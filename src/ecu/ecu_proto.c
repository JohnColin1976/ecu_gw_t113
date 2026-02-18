#include "ecu/ecu_proto.h"
#include "ecu/ecu_limits.h"
#include <stddef.h>

int ecu_hdr_validate(const ecu_hdr_t* h)
{
    if (!h) return 0;
    if (h->magic != (uint16_t)ECU_MAGIC) return 0;
    if (h->version != (uint8_t)ECU_VERSION) return 0;
    if (h->payload_len > (uint16_t)ECU_MAX_PAYLOAD) return 0;
    if (h->reserved1 != 0u) return 0;
    if (h->reserved2 != 0u) return 0;
    return 1;
}

static uint16_t crc16_update_ccitt(uint16_t crc, const uint8_t* data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            if (crc & 0x8000u) crc = (uint16_t)((crc << 1) ^ 0x1021u);
            else              crc = (uint16_t)(crc << 1);
        }
    }
    return crc;
}

uint16_t ecu_frame_calc_crc2(const ecu_hdr_t* h, const uint8_t* payload)
{
    uint16_t crc = 0xFFFFu;
    crc = crc16_update_ccitt(crc, (const uint8_t*)h, sizeof(*h));
    if (h->payload_len && payload) {
        crc = crc16_update_ccitt(crc, payload, (size_t)h->payload_len);
    }
    return crc;
}

int ecu_frame_check_crc(const ecu_hdr_t* h, const uint8_t* payload, uint16_t crc_le)
{
    if (!ecu_hdr_validate(h)) return 0;
    uint16_t calc = ecu_frame_calc_crc2(h, payload);
    return (calc == crc_le) ? 1 : 0;
}
