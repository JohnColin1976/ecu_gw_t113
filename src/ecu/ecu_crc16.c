#include "ecu/ecu_proto.h"
#include <stdint.h>
#include <stddef.h>

uint16_t ecu_crc16_ccitt(const void* data, size_t len)
{
    // CRC-16/CCITT-FALSE: poly=0x1021, init=0xFFFF, xorout=0x0000, refin=false, refout=false
    const uint8_t* p = (const uint8_t*)data;
    uint16_t crc = 0xFFFFu;

    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)p[i] << 8;
        for (int b = 0; b < 8; b++) {
            if (crc & 0x8000u) crc = (uint16_t)((crc << 1) ^ 0x1021u);
            else              crc = (uint16_t)(crc << 1);
        }
    }
    return crc;
}
