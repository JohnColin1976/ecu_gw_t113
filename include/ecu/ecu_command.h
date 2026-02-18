#pragma once
#include <stdint.h>

#ifdef __GNUC__
  #define ECU_PACKED __attribute__((packed))
#else
  #define ECU_PACKED
#endif

// COMMAND payload: [command_id u16][param_len u16][param_data...]
typedef struct ECU_PACKED {
    uint16_t command_id;
    uint16_t param_len;    // N
    // uint8_t param_data[N];  // follows
} ecu_command_hdr_t;

// ACK payload
typedef struct ECU_PACKED {
    uint16_t ack_seq;
    uint16_t status_code;  // 0 OK, 1 UNKNOWN_COMMAND, 2 INVALID_PARAM, 3 INTERNAL_ERROR
} ecu_ack_v1_t;

_Static_assert(sizeof(ecu_ack_v1_t) == 4, "ack_v1 size must be 4");

// TIME_SYNC payload
typedef struct ECU_PACKED {
    uint64_t unix_time_ms;
} ecu_time_sync_v1_t;

_Static_assert(sizeof(ecu_time_sync_v1_t) == 8, "time_sync_v1 size must be 8");

// HELLO payload
typedef struct ECU_PACKED {
    uint8_t  node_id;
    uint32_t fw_version;
    uint32_t build_time;
    uint32_t capabilities_mask;
} ecu_hello_v1_t;

_Static_assert(sizeof(ecu_hello_v1_t) == 13, "hello_v1 size must be 13");

// EVENT payload: [event_code u16][data_len u16][data...]
typedef struct ECU_PACKED {
    uint16_t event_code;
    uint16_t data_len;
    // uint8_t data[data_len];  // follows
} ecu_event_hdr_t;
