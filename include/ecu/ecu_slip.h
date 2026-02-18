#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// RFC1055 SLIP
#define SLIP_END      0xC0
#define SLIP_ESC      0xDB
#define SLIP_ESC_END  0xDC
#define SLIP_ESC_ESC  0xDD

typedef struct {
    uint8_t*  out;       // куда складываем декодированный кадр
    size_t    out_cap;   // ёмкость out
    size_t    out_len;   // сколько накопили
    int       esc;       // был ESC
    int       in_frame;  // видели ли начало (не обязательно, но удобно)
    size_t    frames;    // счётчик принятых кадров
    size_t    drops;     // переполнения/сбросы
} slip_rx_t;

void   slip_rx_init(slip_rx_t* s, uint8_t* out_buf, size_t out_cap);

// Пушим входные байты.
// Возвращает:
//  0  - кадр не завершён
//  1  - кадр завершён, длина в *frame_len (out_len), данные в out[]
// -1  - ошибка (overflow), кадр сброшен
int    slip_rx_push(slip_rx_t* s, const uint8_t* data, size_t len, size_t* frame_len);

// Кодирование кадра в SLIP. Возвращает длину результата, или 0 если out_cap мало.
size_t slip_encode(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_cap);

#ifdef __cplusplus
}
#endif

