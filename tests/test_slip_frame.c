#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "ecu/ecu_limits.h"
#include "ecu/ecu_proto.h"
#include "ecu/ecu_telemetry.h"
#include "ecu/ecu_slip.h"

static void hex_dump(const uint8_t* p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        printf("%02X%s", p[i], ((i + 1) % 16 == 0) ? "\n" : " ");
    }
    if (n % 16) printf("\n");
}

int main(void)
{
    // 1) Собираем TELEMETRY frame bytes: header(16)+payload(24)+crc(2)=42
    ecu_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.magic = ECU_MAGIC;
    h.version = ECU_VERSION;
    h.msg_type = ECU_MSG_TELEMETRY;
    h.src = ECU_NODE2;       // Node 2
    h.dst = ECU_NODE_GW;     // gateway
    h.seq = 100;
    h.flags = 0;
    h.payload_len = (uint16_t)sizeof(ecu_telemetry_v1_t);

    ecu_telemetry_v1_t t;
    t.uptime_ms = 12345678u;
    t.status_flags = 3;
    t.error_code = 0;
    t.voltage = 48.25f;
    t.current = 12.5f;
    t.temperature = 36.75f;
    t.rpm = 2950.0f;

    uint16_t crc = ecu_frame_calc_crc2(&h, (const uint8_t*)&t);

    uint8_t frame[ECU_HEADER_SIZE + sizeof(ecu_telemetry_v1_t) + ECU_CRC_SIZE];
    size_t off = 0;
    memcpy(frame + off, &h, sizeof(h)); off += sizeof(h);
    memcpy(frame + off, &t, sizeof(t)); off += sizeof(t);
    memcpy(frame + off, &crc, sizeof(crc)); off += sizeof(crc);

    printf("RAW ECU frame (%zu bytes):\n", off);
    hex_dump(frame, off);

    // 2) SLIP encode
    uint8_t slip[2048];
    size_t slip_len = slip_encode(frame, off, slip, sizeof(slip));
    if (slip_len == 0) {
        fprintf(stderr, "SLIP encode failed (buffer too small)\n");
        return 1;
    }
    printf("SLIP bytes (%zu bytes):\n", slip_len);
    hex_dump(slip, slip_len);

    // 3) SLIP decode обратно
    uint8_t decoded[2048];
    slip_rx_t rx;
    slip_rx_init(&rx, decoded, sizeof(decoded));

    size_t got_len = 0;
    int r = slip_rx_push(&rx, slip, slip_len, &got_len);
    if (r != 1) {
        fprintf(stderr, "SLIP decode did not yield a frame: r=%d\n", r);
        return 2;
    }

    printf("Decoded SLIP frame (%zu bytes)\n", got_len);
    if (got_len != off) {
        fprintf(stderr, "Length mismatch: got=%zu expected=%zu\n", got_len, off);
        return 3;
    }
    if (memcmp(decoded, frame, off) != 0) {
        fprintf(stderr, "Decoded bytes mismatch\n");
        return 4;
    }

    // 4) Разбираем: header/payload/crc
    if (got_len < ECU_HEADER_SIZE + ECU_CRC_SIZE) {
        fprintf(stderr, "Too short\n");
        return 5;
    }

    const ecu_hdr_t* ph = (const ecu_hdr_t*)decoded;
    if (!ecu_hdr_validate(ph)) {
        fprintf(stderr, "Header validate failed\n");
        return 6;
    }

    size_t expected_len = (size_t)ECU_HEADER_SIZE + (size_t)ph->payload_len + ECU_CRC_SIZE;
    if (got_len != expected_len) {
        fprintf(stderr, "Frame size mismatch: got=%zu expected=%zu\n", got_len, expected_len);
        return 7;
    }

    const uint8_t* payload = decoded + ECU_HEADER_SIZE;
    uint16_t rx_crc;
    memcpy(&rx_crc, decoded + ECU_HEADER_SIZE + ph->payload_len, sizeof(rx_crc));

    if (!ecu_frame_check_crc(ph, payload, rx_crc)) {
        fprintf(stderr, "CRC check failed\n");
        return 8;
    }

    // 5) TELEMETRY payload sanity
    if (ph->msg_type != ECU_MSG_TELEMETRY) {
        fprintf(stderr, "Unexpected msg_type=%u\n", ph->msg_type);
        return 9;
    }
    const ecu_telemetry_v1_t* pt = (const ecu_telemetry_v1_t*)payload;
    printf("OK: node=%u uptime=%u voltage=%.2f current=%.2f temp=%.2f rpm=%.1f\n",
           ph->src, pt->uptime_ms, pt->voltage, pt->current, pt->temperature, pt->rpm);

    return 0;
}
