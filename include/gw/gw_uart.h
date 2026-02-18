#pragma once

#include <stdint.h>
#include <stddef.h>

#include "ecu/ecu_slip.h"

typedef struct {
    int fd;
    const char* dev_path;
    int baud;

    // RX накопитель "сырых байт". Пока без SLIP: просто буфер.
    // На следующем шаге сюда добавим SLIP decoder.
    uint8_t rx_buf[4096];
    size_t  rx_len;

    // SLIP decoder output (1 кадр)
    uint8_t slip_frame[1200]; // ECU_HEADER(16)+payload(1024)+crc(2)=1042, запас
    slip_rx_t slip;

    // TX очередь (кольцевой буфер)
    uint8_t tx_buf[8192];
    size_t  tx_head; // write position
    size_t  tx_tail; // read position
} gw_uart_t;

// Открыть и настроить UART (O_NONBLOCK, raw 8N1)
int gw_uart_open(gw_uart_t* u, const char* dev_path, int baud);

// Закрыть
void gw_uart_close(gw_uart_t* u);

// fd для epoll
int gw_uart_fd(const gw_uart_t* u);

// Можно ли читать/писать
int gw_uart_handle_read(gw_uart_t* u);   // читает в rx_buf, возвращает bytes read или <0
int gw_uart_handle_write(gw_uart_t* u);  // пишет из tx_buf, возвращает bytes written или <0

// Поставить данные в очередь на отправку (не блокирует)
int gw_uart_queue_tx(gw_uart_t* u, const uint8_t* data, size_t len);

// Сколько байт в TX очереди
size_t gw_uart_tx_pending(const gw_uart_t* u);

// Очистить RX буфер (когда обработал)
void gw_uart_rx_consume(gw_uart_t* u, size_t n);

// Пытается извлечь один SLIP-кадр из накопленных rx_buf.
// Возвращает: 1 = кадр получен (data,len), 0 = нет, -1 = ошибка (сброс/мусор)
int gw_uart_try_get_slip_frame(gw_uart_t* u, const uint8_t** data, size_t* len);

// Упаковать ECU-frame bytes (уже с CRC!) в SLIP и поставить в TX очередь
int gw_uart_send_slip(gw_uart_t* u, const uint8_t* frame, size_t frame_len);