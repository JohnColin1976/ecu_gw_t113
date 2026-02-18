#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __GNUC__
  #define ECU_PACKED __attribute__((packed))
#else
  #define ECU_PACKED
#endif

typedef enum {
    ECU_MSG_HELLO     = 0x01,
    ECU_MSG_TELEMETRY = 0x02,
    ECU_MSG_COMMAND   = 0x03,
    ECU_MSG_ACK       = 0x04,
    ECU_MSG_TIME_SYNC = 0x05,
    ECU_MSG_EVENT     = 0x06,
    ECU_MSG_CONFIG    = 0x07,
    ECU_MSG_HEARTBEAT = 0x08
} ecu_msg_type_t;

typedef enum {
    ECU_F_ACK_REQUIRED = (1u << 0),
    ECU_F_IS_ACK       = (1u << 1),
    ECU_F_IS_NACK      = (1u << 2),
    ECU_F_ERROR        = (1u << 3),
    ECU_F_URGENT       = (1u << 4),
} ecu_flags_t;

// Заголовок ECU кадра (16 байт), little-endian
typedef struct ECU_PACKED {
    uint16_t magic;        // 0xEC10
    uint8_t  version;      // 1
    uint8_t  msg_type;     // ecu_msg_type_t
    uint8_t  src;          // NodeID
    uint8_t  dst;          // NodeID
    uint16_t seq;          // seq
    uint16_t flags;        // ecu_flags_t
    uint16_t payload_len;  // bytes
    uint16_t reserved1;    // 0
    uint16_t reserved2;    // 0
} ecu_hdr_t;

// В памяти в T113 можно хранить “собранный кадр”
typedef struct {
    ecu_hdr_t hdr;
    uint8_t   payload[1024];  // ECU_MAX_PAYLOAD
    uint16_t  crc;            // CRC16(header+payload)
} ecu_frame_t;

// CRC16-CCITT (poly 0x1021, init 0xFFFF)
uint16_t ecu_crc16_ccitt(const void* data, size_t len);

// Проверка заголовка на базовую валидность (magic/version/len)
int ecu_hdr_validate(const ecu_hdr_t* h);

// Проверить CRC кадра (возврат 1 = OK, 0 = bad)
int ecu_frame_check_crc(const ecu_hdr_t* h, const uint8_t* payload, uint16_t crc_le);

// Считать CRC
uint16_t ecu_frame_calc_crc2(const ecu_hdr_t* h, const uint8_t* payload);



