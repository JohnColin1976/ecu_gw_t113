#pragma once
#include <stdint.h>
#include <stddef.h>

#ifndef GW_NET_MAX_CLIENTS
#define GW_NET_MAX_CLIENTS 8
#endif

typedef struct {
    int     fd;
    uint8_t rx_buf[8192];
    size_t  rx_len;
} gw_net_client_t;

typedef struct {
    int listen_fd;
    gw_net_client_t clients[GW_NET_MAX_CLIENTS];
} gw_net_t;

int  gw_net_listen(gw_net_t* n, uint16_t port);
void gw_net_close(gw_net_t* n);

int  gw_net_listen_fd(const gw_net_t* n);

// accept all pending clients; returns number accepted, or <0
int  gw_net_accept(gw_net_t* n);

// find client by fd; returns pointer or NULL
gw_net_client_t* gw_net_find_client(gw_net_t* n, int fd);

// remove client (close fd)
void gw_net_remove_client(gw_net_t* n, int fd);

// read into client's rx buffer; returns bytes read, 0 no data, -1 disconnect/error
int  gw_net_client_read(gw_net_client_t* c);

// try extract one frame from client stream (len+frame)
// returns: 1 got frame, 0 not enough, -1 protocol error (drop buffer)
int  gw_net_client_try_get_frame(gw_net_client_t* c, uint8_t* out_frame, size_t out_cap, size_t* out_len);

// send frame to all clients (best-effort; на следующем шаге сделаем TX очереди)
int  gw_net_broadcast_frame(gw_net_t* n, const uint8_t* frame, size_t len);
