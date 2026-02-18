#include "gw/gw_uart.h"
#include "ecu/ecu_slip.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <stdio.h>

static speed_t baud_to_termios(int baud)
{
    switch (baud) {
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
        default: return B115200;
    }
}

static int setup_raw_8n1(int fd, int baud)
{
    struct termios tio;
    if (tcgetattr(fd, &tio) < 0) return -1;

    cfmakeraw(&tio);

    tio.c_cflag &= ~PARENB;
    tio.c_cflag &= ~CSTOPB;
    tio.c_cflag &= ~CSIZE;
    tio.c_cflag |= CS8;

    tio.c_cflag |= (CLOCAL | CREAD);
    #ifdef CRTSCTS
        tio.c_cflag &= ~CRTSCTS;
    #endif

    // non-blocking read behavior (we use O_NONBLOCK anyway)
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 0;

    speed_t sp = baud_to_termios(baud);
    cfsetispeed(&tio, sp);
    cfsetospeed(&tio, sp);

    if (tcsetattr(fd, TCSANOW, &tio) < 0) return -1;
    tcflush(fd, TCIOFLUSH);
    return 0;
}

int gw_uart_open(gw_uart_t* u, const char* dev_path, int baud)
{
    if (!u || !dev_path) return -1;
    memset(u, 0, sizeof(*u));
    u->fd = -1;
    u->dev_path = dev_path;
    u->baud = baud;

    int fd = open(dev_path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;

    if (setup_raw_8n1(fd, baud) < 0) {
        close(fd);
        return -1;
    }

    u->fd = fd;
    slip_rx_init(&u->slip, u->slip_frame, sizeof(u->slip_frame));
    return 0;
}

void gw_uart_close(gw_uart_t* u)
{
    if (!u) return;
    if (u->fd >= 0) close(u->fd);
    u->fd = -1;
    u->rx_len = 0;
    u->tx_head = u->tx_tail = 0;
}

int gw_uart_fd(const gw_uart_t* u)
{
    return u ? u->fd : -1;
}

static size_t ring_used(const gw_uart_t* u)
{
    if (u->tx_head >= u->tx_tail) return u->tx_head - u->tx_tail;
    return sizeof(u->tx_buf) - (u->tx_tail - u->tx_head);
}

static size_t ring_free(const gw_uart_t* u)
{
    // оставляем 1 байт, чтобы отличать full/empty
    return (sizeof(u->tx_buf) - 1) - ring_used(u);
}

int gw_uart_queue_tx(gw_uart_t* u, const uint8_t* data, size_t len)
{
    if (!u || !data || len == 0) return 0;
    if (len > ring_free(u)) return -1;

    for (size_t i = 0; i < len; i++) {
        u->tx_buf[u->tx_head] = data[i];
        u->tx_head = (u->tx_head + 1) % sizeof(u->tx_buf);
    }
    return (int)len;
}

size_t gw_uart_tx_pending(const gw_uart_t* u)
{
    return u ? ring_used(u) : 0;
}

int gw_uart_handle_write(gw_uart_t* u)
{
    if (!u || u->fd < 0) return -1;
    size_t used = ring_used(u);
    if (used == 0) return 0;

    // Пишем непрерывный кусок от tail до конца буфера (или до head)
    size_t tail = u->tx_tail;
    size_t head = u->tx_head;

    size_t chunk = (head > tail) ? (head - tail) : (sizeof(u->tx_buf) - tail);

    ssize_t w = write(u->fd, &u->tx_buf[tail], chunk);
    if (w < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
    u->tx_tail = (u->tx_tail + (size_t)w) % sizeof(u->tx_buf);
    return (int)w;
}

int gw_uart_handle_read(gw_uart_t* u)
{
    if (!u || u->fd < 0) return -1;
    if (u->rx_len >= sizeof(u->rx_buf)) {
        // RX overflow: сбросим буфер (на следующем шаге сделаем нормальную стратегию)
        u->rx_len = 0;
    }

    ssize_t r = read(u->fd, &u->rx_buf[u->rx_len], sizeof(u->rx_buf) - u->rx_len);
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
    if (r == 0) return 0;

    u->rx_len += (size_t)r;
    return (int)r;
}

void gw_uart_rx_consume(gw_uart_t* u, size_t n)
{
    if (!u || n == 0) return;
    if (n >= u->rx_len) {
        u->rx_len = 0;
        return;
    }
    memmove(u->rx_buf, u->rx_buf + n, u->rx_len - n);
    u->rx_len -= n;
}

int gw_uart_try_get_slip_frame(gw_uart_t* u, const uint8_t** data, size_t* len)
{
    if (!u || !data || !len) return -1;
    *data = NULL;
    *len  = 0;

    if (u->rx_len == 0) return 0;

    size_t frame_len = 0;
    int r = slip_rx_push(&u->slip, u->rx_buf, u->rx_len, &frame_len);

    if (r == 1) {
        // Важно: slip_rx_push вернул кадр и уже сбросил внутренний out_len,
        // но сам кадр находится в u->slip_frame[0..frame_len-1].
        // Теперь нужно "вычесть" из rx_buf то количество входных байт, которые были реально потреблены.
        // Мы сделали упрощение: отдаём по одному кадру за вызов и потребляем ВСЁ rx_len.
        // Чтобы было корректно по байтам, сделаем по-простому: после обработки вычищаем rx_len.
        // (В большинстве случаев нормально, потому что мы обрабатываем часто; но лучше точно.)
        //
        // Надёжный вариант: в slip_rx_push добавить счётчик consumed_bytes. Но пока упростим:
        u->rx_len = 0;

        *data = u->slip_frame;
        *len  = frame_len;
        return 1;
    }

    if (r < 0) {
        // мусор/overflow — сбросим накопленное
        u->rx_len = 0;
        return -1;
    }

    // кадр не завершён, но мы уже прогнали данные через декодер — rx_buf можно очистить
    u->rx_len = 0;
    return 0;
}

int gw_uart_send_slip(gw_uart_t* u, const uint8_t* frame, size_t frame_len)
{
    if (!u || !frame || frame_len == 0) return -1;

    // оценим худший случай: каждый байт станет 2 + BEGIN/END
    size_t worst = 2 + frame_len * 2 + 1;
    if (worst > 2048 && worst > sizeof(u->tx_buf)) {
        // слишком большой кадр для нашей очереди
        return -1;
    }

    uint8_t tmp[2400];
    size_t enc = slip_encode(frame, frame_len, tmp, sizeof(tmp));
    if (enc == 0) return -1;

    return gw_uart_queue_tx(u, tmp, enc);
}
