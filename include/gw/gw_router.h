#pragma once
#include <stdint.h>

typedef enum {
    GW_UART_1 = 0,
    GW_UART_4 = 1,
    GW_UART_5 = 2,
    GW_UART_COUNT = 3
} gw_uart_index_t;

int gw_router_node_to_uart(uint8_t node_id, gw_uart_index_t* out_uart);
