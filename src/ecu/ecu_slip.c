#include "ecu/ecu_slip.h"

void slip_rx_init(slip_rx_t* s, uint8_t* out_buf, size_t out_cap)
{
    s->out = out_buf;
    s->out_cap = out_cap;
    s->out_len = 0;
    s->esc = 0;
    s->in_frame = 0;
    s->frames = 0;
    s->drops = 0;
}

static int slip_rx_put(slip_rx_t* s, uint8_t b)
{
    if (s->out_len >= s->out_cap) {
        s->drops++;
        // сброс кадра
        s->out_len = 0;
        s->esc = 0;
        s->in_frame = 0;
        return -1;
    }
    s->out[s->out_len++] = b;
    return 0;
}

int slip_rx_push(slip_rx_t* s, const uint8_t* data, size_t len, size_t* frame_len)
{
    if (frame_len) *frame_len = 0;
    int got_frame = 0;

    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];

        if (b == SLIP_END) {
            if (s->in_frame && s->out_len > 0) {
                // кадр завершён
                if (frame_len) *frame_len = s->out_len;
                s->frames++;
                got_frame = 1;

                // подготовиться к следующему кадру
                s->out_len = 0;
                s->esc = 0;
                s->in_frame = 1;
                return 1; // отдаём по одному кадру за вызов (удобно для обработки)
            } else {
                // пустой END — игнорируем, но считаем что мы "в кадре"
                s->in_frame = 1;
                s->out_len = 0;
                s->esc = 0;
                continue;
            }
        }

        if (!s->in_frame) {
            // ждём первый END как "синхронизацию" (можно убрать, но так надёжнее на мусоре)
            continue;
        }

        if (s->esc) {
            s->esc = 0;
            if (b == SLIP_ESC_END) {
                if (slip_rx_put(s, SLIP_END) < 0) return -1;
            } else if (b == SLIP_ESC_ESC) {
                if (slip_rx_put(s, SLIP_ESC) < 0) return -1;
            } else {
                // некорректная escape-последовательность — сброс кадра
                s->drops++;
                s->out_len = 0;
                s->in_frame = 0;
                return -1;
            }
        } else {
            if (b == SLIP_ESC) {
                s->esc = 1;
            } else {
                if (slip_rx_put(s, b) < 0) return -1;
            }
        }
    }

    return got_frame ? 1 : 0;
}

size_t slip_encode(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_cap)
{
    size_t o = 0;

    // begin
    if (o + 1 > out_cap) return 0;
    out[o++] = SLIP_END;

    for (size_t i = 0; i < in_len; i++) {
        uint8_t b = in[i];
        if (b == SLIP_END) {
            if (o + 2 > out_cap) return 0;
            out[o++] = SLIP_ESC;
            out[o++] = SLIP_ESC_END;
        } else if (b == SLIP_ESC) {
            if (o + 2 > out_cap) return 0;
            out[o++] = SLIP_ESC;
            out[o++] = SLIP_ESC_ESC;
        } else {
            if (o + 1 > out_cap) return 0;
            out[o++] = b;
        }
    }

    // end
    if (o + 1 > out_cap) return 0;
    out[o++] = SLIP_END;

    return o;
}
