#pragma once
#include <stdint.h>

#ifdef __GNUC__
  #define ECU_PACKED __attribute__((packed))
#else
  #define ECU_PACKED
#endif

// TELEMETRY payload v1 (24 bytes)
typedef struct ECU_PACKED {
    uint32_t uptime_ms;     // u32
    uint16_t status_flags;  // u16
    uint16_t error_code;    // u16
    float    voltage;       // float32 IEEE754
    float    current;       // float32
    float    temperature;   // float32
    float    rpm;           // float32
} ecu_telemetry_v1_t;

_Static_assert(sizeof(ecu_telemetry_v1_t) == 24, "telemetry_v1 size must be 24");


