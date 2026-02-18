#include "gw/gw_app.h"

#include "gw/gw_net.h"
#include "gw/gw_uart.h"
#include "gw/gw_router.h"

#include "ecu/ecu_limits.h"
#include "ecu/ecu_proto.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>

#define GW_TCP_PORT 9100
#define GW_BAUD     115200

static int ep_add(int ep, int fd, uint32_t events)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.fd = fd;
    return epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
}

static int ep_mod(int ep, int fd, uint32_t events)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.fd = fd;
    return epoll_ctl(ep, EPOLL_CTL_MOD, fd, &ev);
}

static int is_uart_fd(const gw_uart_t uarts[GW_UART_COUNT], int fd, int* out_idx)
{
    for (int i = 0; i < GW_UART_COUNT; i++) {
        if (gw_uart_fd(&uarts[i]) == fd) {
            if (out_idx) *out_idx = i;
            return 1;
        }
    }
    return 0;
}

static uint32_t uart_events_mask(const gw_uart_t* u)
{
    uint32_t ev = EPOLLIN;
    if (gw_uart_tx_pending(u) > 0) ev |= EPOLLOUT;
    return ev;
}

static int validate_ecu_bytes(const uint8_t* frame, size_t frame_len,
                              const ecu_hdr_t** out_hdr,
                              const uint8_t** out_payload)
{
    if (frame_len < ECU_HEADER_SIZE + ECU_CRC_SIZE) return 0;

    const ecu_hdr_t* h = (const ecu_hdr_t*)frame;
    if (!ecu_hdr_validate(h)) return 0;

    size_t need = (size_t)ECU_HEADER_SIZE + (size_t)h->payload_len + ECU_CRC_SIZE;
    if (frame_len != need) return 0;

    const uint8_t* payload = frame + ECU_HEADER_SIZE;
    uint16_t crc_le;
    memcpy(&crc_le, frame + ECU_HEADER_SIZE + h->payload_len, sizeof(crc_le));

    if (!ecu_frame_check_crc(h, payload, crc_le)) return 0;

    if (out_hdr) *out_hdr = h;
    if (out_payload) *out_payload = payload;
    return 1;
}

int gw_app_run(void)
{
    // 1) UARTs
    gw_uart_t uarts[GW_UART_COUNT];
    if (gw_uart_open(&uarts[GW_UART_1], "/dev/ttyS1", GW_BAUD) < 0) {
        perror("open /dev/ttyS1");
        return 1;
    }
    if (gw_uart_open(&uarts[GW_UART_4], "/dev/ttyS4", GW_BAUD) < 0) {
        perror("open /dev/ttyS4");
        return 1;
    }
    if (gw_uart_open(&uarts[GW_UART_5], "/dev/ttyS5", GW_BAUD) < 0) {
        perror("open /dev/ttyS5");
        return 1;
    }

    // 2) NET listen
    gw_net_t net;
    if (gw_net_listen(&net, GW_TCP_PORT) < 0) {
        perror("gw_net_listen");
        return 1;
    }

    // 3) epoll
    int ep = epoll_create1(0);
    if (ep < 0) {
        perror("epoll_create1");
        return 1;
    }

    // add listen fd
    if (ep_add(ep, gw_net_listen_fd(&net), EPOLLIN) < 0) {
        perror("epoll add listen");
        return 1;
    }

    // add uart fds
    for (int i = 0; i < GW_UART_COUNT; i++) {
        int fd = gw_uart_fd(&uarts[i]);
        if (ep_add(ep, fd, uart_events_mask(&uarts[i])) < 0) {
            perror("epoll add uart");
            return 1;
        }
    }

    fprintf(stderr, "ecu-gw: TCP :%d, UARTs: ttyS1 ttyS4 ttyS5 @ %d\n", GW_TCP_PORT, GW_BAUD);

    uint8_t net_frame[ECU_MAX_FRAME_SIZE];

    for (;;) {
        struct epoll_event evs[16];
        int n = epoll_wait(ep, evs, 16, 100);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = evs[i].data.fd;
            uint32_t e = evs[i].events;

            // 3.1) new client(s)
            if (fd == gw_net_listen_fd(&net)) {
                int acc = gw_net_accept(&net);
                if (acc < 0) perror("accept");
                // добавить новых клиентов в epoll
                for (int k = 0; k < GW_NET_MAX_CLIENTS; k++) {
                    int cfd = net.clients[k].fd;
                    if (cfd >= 0) {
                        // add may fail if already added; ignore
                        ep_add(ep, cfd, EPOLLIN);
                    }
                }
                continue;
            }

            // 3.2) UART events
            int uart_idx = -1;
            if (is_uart_fd(uarts, fd, &uart_idx)) {
                gw_uart_t* u = &uarts[uart_idx];

                if (e & EPOLLIN) {
                    int rr = gw_uart_handle_read(u);
                    if (rr < 0) {
                        fprintf(stderr, "UART read error on %s\n", u->dev_path);
                    } else {
                        // вытащить SLIP кадры (по одному/несколько)
                        for (;;) {
                            const uint8_t* f = NULL;
                            size_t flen = 0;
                            int gr = gw_uart_try_get_slip_frame(u, &f, &flen);
                            if (gr == 0) break;
                            if (gr < 0) break;

                            const ecu_hdr_t* h = NULL;
                            if (!validate_ecu_bytes(f, flen, &h, NULL)) {
                                fprintf(stderr, "UART %s: bad ECU frame (drop)\n", u->dev_path);
                                continue;
                            }
                            // отправить на ПК всем клиентам
                            gw_net_broadcast_frame(&net, f, flen);
                        }
                    }
                }

                if (e & EPOLLOUT) {
                    int wr = gw_uart_handle_write(u);
                    if (wr < 0) {
                        fprintf(stderr, "UART write error on %s\n", u->dev_path);
                    }
                }

                // обновить маску EPOLLOUT в зависимости от очереди
                ep_mod(ep, fd, uart_events_mask(u));
                continue;
            }

            // 3.3) client socket events
            gw_net_client_t* c = gw_net_find_client(&net, fd);
            if (c) {
                if (e & EPOLLIN) {
                    int rr = gw_net_client_read(c);
                    if (rr < 0) {
                        gw_net_remove_client(&net, fd);
                        continue;
                    }

                    // обработать все полные кадры в буфере
                    for (;;) {
                        size_t flen = 0;
                        int gr = gw_net_client_try_get_frame(c, net_frame, sizeof(net_frame), &flen);
                        if (gr == 0) break;
                        if (gr < 0) break;

                        const ecu_hdr_t* h = NULL;
                        if (!validate_ecu_bytes(net_frame, flen, &h, NULL)) {
                            fprintf(stderr, "NET: bad ECU frame (drop)\n");
                            continue;
                        }

                        // роутинг на UART по dst
                        gw_uart_index_t out;
                        if (!gw_router_node_to_uart(h->dst, &out)) {
                            // dst может быть GW/broadcast — пока игнорируем
                            continue;
                        }

                        // отправить на UART (SLIP)
                        (void)gw_uart_send_slip(&uarts[out], net_frame, flen);
                        // включить EPOLLOUT если нужно
                        ep_mod(ep, gw_uart_fd(&uarts[out]), uart_events_mask(&uarts[out]));
                    }
                }
                continue;
            }
        }
    }

    close(ep);
    gw_net_close(&net);
    for (int i = 0; i < GW_UART_COUNT; i++) gw_uart_close(&uarts[i]);
    return 0;
}
