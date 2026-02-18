#include "gw/gw_router.h"
#include "ecu/ecu_limits.h"

int gw_router_node_to_uart(uint8_t node_id, gw_uart_index_t* out_uart)
{
    if (!out_uart) return 0;

    switch (node_id) {
        case ECU_NODE1: *out_uart = GW_UART_1; return 1; // ttyS1
        case ECU_NODE2: *out_uart = GW_UART_4; return 1; // ttyS4
        case ECU_NODE3: *out_uart = GW_UART_5; return 1; // ttyS5
        default: return 0;
    }
}
