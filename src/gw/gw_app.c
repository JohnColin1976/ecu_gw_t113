#include "gw/gw_app.h"

#include "gw/gw_net.h"
#include "gw/gw_uart.h"
#include "gw/gw_router.h"
#include "gw/gw_cmd_ui.h"

#include "ecu/ecu_limits.h"
#include "ecu/ecu_proto.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
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

static void dump_hex(const char* tag, const uint8_t* data, size_t len)
{
    fprintf(stderr, "%s len=%zu: ", tag, len);
    for (size_t i = 0; i < len; i++) {
        fprintf(stderr, "%02X", data[i]);
        if (i + 1 < len) fputc(' ', stderr);
    }
    fputc('\n', stderr);
}

static void dump_hex_with_port(const char* tag, const char* port_name, const uint8_t* data, size_t len)
{
    fprintf(stderr, "%s [%s] len=%zu: ", tag, port_name ? port_name : "unknown", len);
    for (size_t i = 0; i < len; i++) {
        fprintf(stderr, "%02X", data[i]);
        if (i + 1 < len) fputc(' ', stderr);
    }
    fputc('\n', stderr);
}

static const char* net_peer_name(int fd, char* buf, size_t buf_len)
{
    if (!buf || buf_len == 0) return "unknown";
    buf[0] = '\0';

    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    if (getpeername(fd, (struct sockaddr*)&sa, &sl) == 0) {
        char ip[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &sa.sin_addr, ip, sizeof(ip))) {
            snprintf(buf, buf_len, "%s:%u", ip, (unsigned)ntohs(sa.sin_port));
            return buf;
        }
    }

    snprintf(buf, buf_len, "fd=%d", fd);
    return buf;
}

static int port_token_to_uart(const char* tok, gw_uart_index_t* out_idx)
{
    if (!tok || !out_idx) return 0;

    if (strcmp(tok, "1") == 0 || strcasecmp(tok, "ttyS1") == 0 || strcasecmp(tok, "/dev/ttyS1") == 0) {
        *out_idx = GW_UART_1;
        return 1;
    }
    if (strcmp(tok, "4") == 0 || strcasecmp(tok, "ttyS4") == 0 || strcasecmp(tok, "/dev/ttyS4") == 0) {
        *out_idx = GW_UART_4;
        return 1;
    }
    if (strcmp(tok, "5") == 0 || strcasecmp(tok, "ttyS5") == 0 || strcasecmp(tok, "/dev/ttyS5") == 0) {
        *out_idx = GW_UART_5;
        return 1;
    }
    return 0;
}

static int parse_send_ports(const char* spec, uint8_t* out_mask)
{
    if (!spec || !out_mask) return -1;
    *out_mask = 0;

    if (strcasecmp(spec, "all") == 0) {
        *out_mask = (uint8_t)((1u << GW_UART_1) | (1u << GW_UART_4) | (1u << GW_UART_5));
        return 0;
    }

    char tmp[128];
    size_t n = strlen(spec);
    if (n == 0 || n >= sizeof(tmp)) return -1;
    memcpy(tmp, spec, n + 1);

    char* saveptr = NULL;
    char* tok = strtok_r(tmp, "_", &saveptr);
    while (tok) {
        gw_uart_index_t idx;
        if (!port_token_to_uart(tok, &idx)) return -1;
        *out_mask = (uint8_t)(*out_mask | (uint8_t)(1u << idx));
        tok = strtok_r(NULL, "_", &saveptr);
    }

    return (*out_mask != 0) ? 0 : -1;
}

static uint8_t uart_to_node(gw_uart_index_t idx)
{
    switch (idx) {
        case GW_UART_1: return ECU_NODE1;
        case GW_UART_4: return ECU_NODE2;
        case GW_UART_5: return ECU_NODE3;
        default: return ECU_NODE_BROADCAST;
    }
}

static int gw_app_send_test(const char* ports_spec, int show_packets)
{
    uint8_t mask = 0;
    if (parse_send_ports(ports_spec, &mask) < 0) {
        fprintf(stderr, "Invalid PORT format for -send_test: %s (use all or list like 1_4_5)\n", ports_spec ? ports_spec : "");
        return 2;
    }

    const char* devs[GW_UART_COUNT] = {"/dev/ttyS1", "/dev/ttyS4", "/dev/ttyS5"};
    gw_uart_t uarts[GW_UART_COUNT];
    memset(uarts, 0, sizeof(uarts));

    uint16_t seq = 1;
    int sent_count = 0;

    for (int i = 0; i < GW_UART_COUNT; i++) {
        if ((mask & (uint8_t)(1u << i)) == 0) continue;

        if (gw_uart_open(&uarts[i], devs[i], GW_BAUD) < 0) {
            perror(devs[i]);
            continue;
        }

        ecu_hdr_t h;
        memset(&h, 0, sizeof(h));
        h.magic = ECU_MAGIC;
        h.version = ECU_VERSION;
        h.msg_type = ECU_MSG_HEARTBEAT;
        h.src = ECU_NODE_GW;
        h.dst = uart_to_node((gw_uart_index_t)i);
        h.seq = seq++;
        h.flags = 0;
        h.payload_len = 0;

        uint16_t crc = ecu_frame_calc_crc2(&h, NULL);
        uint8_t frame[ECU_HEADER_SIZE + ECU_CRC_SIZE];
        memcpy(frame, &h, ECU_HEADER_SIZE);
        memcpy(frame + ECU_HEADER_SIZE, &crc, ECU_CRC_SIZE);

        if (show_packets) dump_hex_with_port("TEST ECU", devs[i], frame, sizeof(frame));

        if (gw_uart_send_slip(&uarts[i], frame, sizeof(frame)) < 0) {
            fprintf(stderr, "Failed to enqueue test frame for %s\n", devs[i]);
            gw_uart_close(&uarts[i]);
            continue;
        }

        int ok = 1;
        for (int tries = 0; tries < 100 && gw_uart_tx_pending(&uarts[i]) > 0; tries++) {
            int wr = gw_uart_handle_write(&uarts[i]);
            if (wr < 0) {
                fprintf(stderr, "Write failed for %s\n", devs[i]);
                ok = 0;
                break;
            }
            if (wr == 0) usleep(5000);
        }

        if (gw_uart_tx_pending(&uarts[i]) > 0) {
            fprintf(stderr, "Timeout sending test frame on %s\n", devs[i]);
            ok = 0;
        }

        if (ok) {
            fprintf(stderr, "Test frame sent on %s\n", devs[i]);
            sent_count++;
        }

        gw_uart_close(&uarts[i]);
    }

    return (sent_count > 0) ? 0 : 1;
}

int gw_app_run(int show_packets, int preview_raw, const char* send_test_ports, const char* cmd_ui_port)
{
    if (cmd_ui_port && cmd_ui_port[0] != '\0') {
        return gw_cmd_ui_run(cmd_ui_port, show_packets, preview_raw);
    }

    if (send_test_ports && send_test_ports[0] != '\0') {
        return gw_app_send_test(send_test_ports, show_packets);
    }

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
                        if (preview_raw && rr > 0 && (size_t)rr <= u->rx_len) {
                            dump_hex_with_port("RAW UART", u->dev_path,
                                               &u->rx_buf[u->rx_len - (size_t)rr], (size_t)rr);
                        }

                        // вытащить SLIP кадры (по одному/несколько)
                        for (;;) {
                            const uint8_t* f = NULL;
                            size_t flen = 0;
                            int gr = gw_uart_try_get_slip_frame(u, &f, &flen);
                            if (gr == 0) break;
                            if (gr < 0) break;

                            if (show_packets) dump_hex("RX UART", f, flen);

                            const ecu_hdr_t* h = NULL;
                            if (!validate_ecu_bytes(f, flen, &h, NULL)) {
                                fprintf(stderr, "UART %s: bad ECU frame (drop)\n", u->dev_path);
                                continue;
                            }
                            // отправить на ПК всем клиентам
                            gw_net_broadcast_frame(&net, f, flen);
                            if (show_packets) dump_hex("PROC UART->NET", f, flen);
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
                    } else if (preview_raw && rr > 0 && (size_t)rr <= c->rx_len) {
                        char peer[64];
                        dump_hex_with_port("RAW NET", net_peer_name(c->fd, peer, sizeof(peer)),
                                           &c->rx_buf[c->rx_len - (size_t)rr], (size_t)rr);
                    }

                    // обработать все полные кадры в буфере
                    for (;;) {
                        size_t flen = 0;
                        int gr = gw_net_client_try_get_frame(c, net_frame, sizeof(net_frame), &flen);
                        if (gr == 0) break;
                        if (gr < 0) break;

                        if (show_packets) dump_hex("RX NET", net_frame, flen);

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
                        if (show_packets) dump_hex("PROC NET->UART", net_frame, flen);
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
