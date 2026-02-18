#include "gw/gw_net.h"
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>

static int set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

int gw_net_listen_fd(const gw_net_t* n) { return n ? n->listen_fd : -1; }

int gw_net_listen(gw_net_t* n, uint16_t port)
{
    if (!n) return -1;
    memset(n, 0, sizeof(*n));
    n->listen_fd = -1;
    for (int i = 0; i < GW_NET_MAX_CLIENTS; i++) n->clients[i].fd = -1;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);

    if (bind(fd, (struct sockaddr*)&a, sizeof(a)) < 0) { close(fd); return -1; }
    if (listen(fd, 8) < 0) { close(fd); return -1; }
    if (set_nonblock(fd) < 0) { close(fd); return -1; }

    n->listen_fd = fd;
    return 0;
}

void gw_net_close(gw_net_t* n)
{
    if (!n) return;
    if (n->listen_fd >= 0) close(n->listen_fd);
    n->listen_fd = -1;

    for (int i = 0; i < GW_NET_MAX_CLIENTS; i++) {
        if (n->clients[i].fd >= 0) close(n->clients[i].fd);
        n->clients[i].fd = -1;
        n->clients[i].rx_len = 0;
    }
}

gw_net_client_t* gw_net_find_client(gw_net_t* n, int fd)
{
    if (!n) return NULL;
    for (int i = 0; i < GW_NET_MAX_CLIENTS; i++) {
        if (n->clients[i].fd == fd) return &n->clients[i];
    }
    return NULL;
}

void gw_net_remove_client(gw_net_t* n, int fd)
{
    if (!n) return;
    for (int i = 0; i < GW_NET_MAX_CLIENTS; i++) {
        if (n->clients[i].fd == fd) {
            close(n->clients[i].fd);
            n->clients[i].fd = -1;
            n->clients[i].rx_len = 0;
            return;
        }
    }
}

int gw_net_accept(gw_net_t* n)
{
    if (!n || n->listen_fd < 0) return -1;

    int accepted = 0;
    for (;;) {
        int c = accept(n->listen_fd, NULL, NULL);
        if (c < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return -1;
        }
        set_nonblock(c);

        int placed = 0;
        for (int i = 0; i < GW_NET_MAX_CLIENTS; i++) {
            if (n->clients[i].fd < 0) {
                n->clients[i].fd = c;
                n->clients[i].rx_len = 0;
                placed = 1;
                accepted++;
                break;
            }
        }
        if (!placed) close(c);
    }
    return accepted;
}

int gw_net_client_read(gw_net_client_t* c)
{
    if (!c || c->fd < 0) return -1;
    if (c->rx_len >= sizeof(c->rx_buf)) c->rx_len = 0;

    ssize_t r = read(c->fd, c->rx_buf + c->rx_len, sizeof(c->rx_buf) - c->rx_len);
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
    if (r == 0) return -1; // disconnect
    c->rx_len += (size_t)r;
    return (int)r;
}

static uint32_t read_u32_le(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int gw_net_client_try_get_frame(gw_net_client_t* c, uint8_t* out_frame, size_t out_cap, size_t* out_len)
{
    if (!c || !out_frame || !out_len) return -1;
    *out_len = 0;

    if (c->rx_len < 4) return 0;

    uint32_t L = read_u32_le(c->rx_buf);
    if (L == 0 || L > out_cap) {
        c->rx_len = 0; // протокольная ошибка — сброс
        return -1;
    }
    if (c->rx_len < 4u + (size_t)L) return 0;

    memcpy(out_frame, c->rx_buf + 4, L);
    *out_len = (size_t)L;

    size_t remain = c->rx_len - (4u + (size_t)L);
    memmove(c->rx_buf, c->rx_buf + 4u + (size_t)L, remain);
    c->rx_len = remain;

    return 1;
}

int gw_net_broadcast_frame(gw_net_t* n, const uint8_t* frame, size_t len)
{
    if (!n || !frame || len == 0) return -1;

    uint8_t hdr[4];
    hdr[0] = (uint8_t)(len & 0xFF);
    hdr[1] = (uint8_t)((len >> 8) & 0xFF);
    hdr[2] = (uint8_t)((len >> 16) & 0xFF);
    hdr[3] = (uint8_t)((len >> 24) & 0xFF);

    for (int i = 0; i < GW_NET_MAX_CLIENTS; i++) {
        int fd = n->clients[i].fd;
        if (fd < 0) continue;
        (void)write(fd, hdr, 4);
        (void)write(fd, frame, len);
    }
    return 0;
}
