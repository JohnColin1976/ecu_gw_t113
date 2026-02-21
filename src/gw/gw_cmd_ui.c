#include "gw/gw_cmd_ui.h"

#include "gw/gw_uart.h"

#include "ecu/ecu_command.h"
#include "ecu/ecu_limits.h"
#include "ecu/ecu_proto.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define CMD_UI_INPUT_MAX   256
#define CMD_UI_RX_LINES    256
#define CMD_UI_LINE_MAX    192
#define CMD_UI_FRAME_MAX   ECU_MAX_FRAME_SIZE

typedef struct {
    char lines[CMD_UI_RX_LINES][CMD_UI_LINE_MAX];
    size_t count;
    size_t head;
    char input[CMD_UI_INPUT_MAX];
    size_t input_len;
    char status[CMD_UI_LINE_MAX];
    int rows;
    int cols;
    const char* port_name;
} cmd_ui_t;

typedef struct {
    struct termios old_tio;
    int old_flags;
    int active;
} term_guard_t;

static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int signo)
{
    (void)signo;
    g_stop = 1;
}

static uint32_t uart_events_mask(const gw_uart_t* u)
{
    uint32_t ev = EPOLLIN;
    if (gw_uart_tx_pending(u) > 0) ev |= EPOLLOUT;
    return ev;
}

static int validate_ecu_bytes(const uint8_t* frame, size_t frame_len, const ecu_hdr_t** out_hdr, const uint8_t** out_payload)
{
    if (frame_len < ECU_HEADER_SIZE + ECU_CRC_SIZE) return 0;

    const ecu_hdr_t* h = (const ecu_hdr_t*)frame;
    if (!ecu_hdr_validate(h)) return 0;

    size_t need = (size_t)ECU_HEADER_SIZE + (size_t)h->payload_len + ECU_CRC_SIZE;
    if (frame_len != need) return 0;

    const uint8_t* payload = frame + ECU_HEADER_SIZE;
    uint16_t crc_le = 0;
    memcpy(&crc_le, frame + ECU_HEADER_SIZE + h->payload_len, sizeof(crc_le));
    if (!ecu_frame_check_crc(h, payload, crc_le)) return 0;

    if (out_hdr) *out_hdr = h;
    if (out_payload) *out_payload = payload;
    return 1;
}

static void now_hms(char out[16])
{
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    snprintf(out, 16, "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

static void ui_add_rx_line(cmd_ui_t* ui, const char* fmt, ...)
{
    if (!ui || !fmt) return;

    char line[CMD_UI_LINE_MAX];
    char tbuf[16];
    now_hms(tbuf);

    int off = snprintf(line, sizeof(line), "[%s] ", tbuf);
    if (off < 0) return;
    if (off >= (int)sizeof(line)) off = (int)sizeof(line) - 1;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + off, sizeof(line) - (size_t)off, fmt, ap);
    va_end(ap);

    size_t idx = ui->head % CMD_UI_RX_LINES;
    memcpy(ui->lines[idx], line, sizeof(ui->lines[idx]));
    ui->head = (ui->head + 1) % CMD_UI_RX_LINES;
    if (ui->count < CMD_UI_RX_LINES) ui->count++;
}

static void ui_set_status(cmd_ui_t* ui, const char* fmt, ...)
{
    if (!ui || !fmt) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ui->status, sizeof(ui->status), fmt, ap);
    va_end(ap);
}

static void ui_get_term_size(cmd_ui_t* ui)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        ui->rows = (int)ws.ws_row;
        ui->cols = (int)ws.ws_col;
        return;
    }
    ui->rows = 24;
    ui->cols = 80;
}

static void ui_draw_sep(int cols)
{
    for (int i = 0; i < cols; i++) fputc('-', stdout);
    fputc('\n', stdout);
}

static void ui_redraw(cmd_ui_t* ui)
{
    if (!ui) return;
    ui_get_term_size(ui);

    int cols = ui->cols > 20 ? ui->cols : 20;
    int rows = ui->rows > 10 ? ui->rows : 10;
    int input_area = 6;
    int rx_rows = rows - input_area - 2;
    if (rx_rows < 1) rx_rows = 1;

    printf("\x1b[2J\x1b[H");
    printf("ECU CMD UI  port=%s  (q/Ctrl+C exit)\n", ui->port_name);
    printf("Format: cmd <command_id> [hex bytes]  |  src \\\\x55\\\\xAA...\n");
    printf("Example: cmd 7 | cmd 2 00 10 | src \\\\x55\\\\xAA\n");
    ui_draw_sep(cols);
    printf("Command> %s\n", ui->input);
    printf("Status : %s\n", ui->status[0] ? ui->status : "ready");
    ui_draw_sep(cols);
    printf("RX area (%d lines)\n", rx_rows);

    size_t to_show = ui->count < (size_t)rx_rows ? ui->count : (size_t)rx_rows;
    size_t start = (ui->head + CMD_UI_RX_LINES - to_show) % CMD_UI_RX_LINES;
    for (size_t i = 0; i < to_show; i++) {
        size_t idx = (start + i) % CMD_UI_RX_LINES;
        printf("%s\n", ui->lines[idx]);
    }
    for (size_t i = to_show; i < (size_t)rx_rows; i++) {
        fputc('\n', stdout);
    }

    fflush(stdout);
}

static void uart_epoll_refresh(int ep, gw_uart_t* uart)
{
    struct epoll_event mev;
    memset(&mev, 0, sizeof(mev));
    mev.events = uart_events_mask(uart);
    mev.data.fd = gw_uart_fd(uart);
    (void)epoll_ctl(ep, EPOLL_CTL_MOD, gw_uart_fd(uart), &mev);
}

static int set_stdin_raw(term_guard_t* tg)
{
    if (!tg) return -1;
    memset(tg, 0, sizeof(*tg));

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return -1;
    if (tcgetattr(STDIN_FILENO, &tg->old_tio) < 0) return -1;
    tg->old_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (tg->old_flags < 0) return -1;

    struct termios tio = tg->old_tio;
    tio.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &tio) < 0) return -1;

    if (fcntl(STDIN_FILENO, F_SETFL, tg->old_flags | O_NONBLOCK) < 0) {
        tcsetattr(STDIN_FILENO, TCSANOW, &tg->old_tio);
        return -1;
    }

    tg->active = 1;
    return 0;
}

static void restore_stdin(term_guard_t* tg)
{
    if (!tg || !tg->active) return;
    (void)tcsetattr(STDIN_FILENO, TCSANOW, &tg->old_tio);
    (void)fcntl(STDIN_FILENO, F_SETFL, tg->old_flags);
    tg->active = 0;
}

static int port_to_uart(const char* port_name, const char** out_dev, uint8_t* out_dst)
{
    if (!port_name || !out_dev || !out_dst) return 0;
    if (strcmp(port_name, "ttyS1") == 0) {
        *out_dev = "/dev/ttyS1";
        *out_dst = ECU_NODE1;
        return 1;
    }
    if (strcmp(port_name, "ttyS4") == 0) {
        *out_dev = "/dev/ttyS4";
        *out_dst = ECU_NODE2;
        return 1;
    }
    if (strcmp(port_name, "ttyS5") == 0) {
        *out_dev = "/dev/ttyS5";
        *out_dst = ECU_NODE3;
        return 1;
    }
    return 0;
}

static int parse_u16_anybase(const char* s, uint16_t* out)
{
    if (!s || !*s || !out) return 0;
    char* end = NULL;
    errno = 0;
    unsigned long v = strtoul(s, &end, 0);
    if (errno != 0 || !end || *end != '\0' || v > 0xFFFFul) return 0;
    *out = (uint16_t)v;
    return 1;
}

static int parse_hex_u8(const char* s, uint8_t* out)
{
    if (!s || !*s || !out) return 0;
    char* end = NULL;
    errno = 0;
    unsigned long v = strtoul(s, &end, 16);
    if (errno != 0 || !end || *end != '\0' || v > 0xFFul) return 0;
    *out = (uint8_t)v;
    return 1;
}

static int is_hex_ch(char c)
{
    return ((c >= '0' && c <= '9') ||
            (c >= 'a' && c <= 'f') ||
            (c >= 'A' && c <= 'F')) ? 1 : 0;
}

static int parse_src_line(const char* line, uint8_t* out_bytes, size_t* out_len, char* err, size_t err_len)
{
    if (!line || !out_bytes || !out_len || !err || err_len == 0) return 0;

    const char* p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (strncasecmp(p, "src", 3) != 0 || !(p[3] == '\0' || p[3] == ' ' || p[3] == '\t')) {
        snprintf(err, err_len, "expected: src \\\\xHH\\\\xHH...");
        return 0;
    }
    p += 3;

    size_t n = 0;
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;

        if (p[0] != '\\' || (p[1] != 'x' && p[1] != 'X') || !is_hex_ch(p[2]) || !is_hex_ch(p[3])) {
            snprintf(err, err_len, "invalid token near: %s", p);
            return 0;
        }
        if (n >= ECU_MAX_FRAME_SIZE) {
            snprintf(err, err_len, "too many bytes (max %u)", (unsigned)ECU_MAX_FRAME_SIZE);
            return 0;
        }

        char hex[3];
        hex[0] = p[2];
        hex[1] = p[3];
        hex[2] = '\0';
        out_bytes[n++] = (uint8_t)strtoul(hex, NULL, 16);
        p += 4;
    }

    if (n == 0) {
        snprintf(err, err_len, "empty src payload");
        return 0;
    }
    *out_len = n;
    return 1;
}

static int parse_cmd_line(const char* line, uint16_t* out_command_id, uint8_t* out_params, size_t* out_param_len, char* err, size_t err_len)
{
    if (!line || !out_command_id || !out_params || !out_param_len || !err || err_len == 0) return 0;
    err[0] = '\0';

    char tmp[CMD_UI_INPUT_MAX];
    size_t n = strnlen(line, sizeof(tmp));
    if (n == 0 || n >= sizeof(tmp)) {
        snprintf(err, err_len, "empty input");
        return 0;
    }
    memcpy(tmp, line, n);
    tmp[n] = '\0';

    char* save = NULL;
    char* tok = strtok_r(tmp, " \t", &save);
    if (!tok) {
        snprintf(err, err_len, "empty input");
        return 0;
    }
    if (strcasecmp(tok, "cmd") != 0) {
        snprintf(err, err_len, "expected: cmd <id> [hex bytes]");
        return 0;
    }

    char* id_tok = strtok_r(NULL, " \t", &save);
    uint16_t command_id = 0;
    if (!id_tok || !parse_u16_anybase(id_tok, &command_id)) {
        snprintf(err, err_len, "invalid command_id");
        return 0;
    }
    if (command_id < 1 || command_id > 8) {
        snprintf(err, err_len, "command_id must be in range 1..8");
        return 0;
    }

    size_t param_len = 0;
    char* p = strtok_r(NULL, " \t", &save);
    while (p) {
        uint8_t b = 0;
        if (!parse_hex_u8(p, &b)) {
            snprintf(err, err_len, "invalid hex byte: %s", p);
            return 0;
        }
        if (sizeof(ecu_command_hdr_t) + param_len + 1 > ECU_MAX_PAYLOAD) {
            snprintf(err, err_len, "payload exceeds %u bytes", (unsigned)ECU_MAX_PAYLOAD);
            return 0;
        }
        out_params[param_len++] = b;
        p = strtok_r(NULL, " \t", &save);
    }

    if (command_id == 8 && param_len != 0) {
        snprintf(err, err_len, "ENTER_BOOT (id=8) requires zero params");
        return 0;
    }

    *out_command_id = command_id;
    *out_param_len = param_len;
    return 1;
}

static int build_command_frame(uint16_t seq, uint8_t dst, uint16_t command_id, const uint8_t* params, size_t param_len,
                               uint8_t* out_frame, size_t out_cap, size_t* out_len)
{
    if (!out_frame || !out_len) return 0;
    if (sizeof(ecu_command_hdr_t) + param_len > ECU_MAX_PAYLOAD) return 0;

    ecu_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.magic = ECU_MAGIC;
    h.version = ECU_VERSION;
    h.msg_type = ECU_MSG_COMMAND;
    h.src = ECU_NODE_GW;
    h.dst = dst;
    h.seq = seq;
    h.flags = ECU_F_ACK_REQUIRED;
    h.payload_len = (uint16_t)(sizeof(ecu_command_hdr_t) + param_len);

    uint8_t payload[ECU_MAX_PAYLOAD];
    ecu_command_hdr_t ch;
    ch.command_id = command_id;
    ch.param_len = (uint16_t)param_len;
    memcpy(payload, &ch, sizeof(ch));
    if (param_len > 0 && params) memcpy(payload + sizeof(ch), params, param_len);

    size_t frame_len = ECU_HEADER_SIZE + (size_t)h.payload_len + ECU_CRC_SIZE;
    if (frame_len > out_cap) return 0;

    uint16_t crc = ecu_frame_calc_crc2(&h, payload);
    memcpy(out_frame, &h, ECU_HEADER_SIZE);
    memcpy(out_frame + ECU_HEADER_SIZE, payload, h.payload_len);
    memcpy(out_frame + ECU_HEADER_SIZE + h.payload_len, &crc, ECU_CRC_SIZE);
    *out_len = frame_len;
    return 1;
}

static void format_hex_preview(const char* tag, const uint8_t* data, size_t len, char* out, size_t out_len)
{
    if (!out || out_len == 0) return;
    size_t off = 0;
    int n = snprintf(out, out_len, "%s len=%zu: ", tag, len);
    if (n < 0) {
        out[0] = '\0';
        return;
    }
    off = (size_t)n < out_len ? (size_t)n : out_len - 1;

    for (size_t i = 0; i < len && off + 3 < out_len; i++) {
        n = snprintf(out + off, out_len - off, "%02X", data[i]);
        if (n < 0) break;
        off += (size_t)n;
        if (i + 1 < len && off + 2 < out_len) out[off++] = ' ';
    }
    out[off] = '\0';
}

static void format_src_bytes(const uint8_t* data, size_t len, char* out, size_t out_len)
{
    if (!out || out_len == 0) return;
    size_t off = 0;
    int n = snprintf(out, out_len, "SRC: [");
    if (n < 0) {
        out[0] = '\0';
        return;
    }
    off = (size_t)n < out_len ? (size_t)n : out_len - 1;

    for (size_t i = 0; i < len && off + 4 < out_len; i++) {
        n = snprintf(out + off, out_len - off, "%02X", data[i]);
        if (n < 0) break;
        off += (size_t)n;
        if (i + 1 < len && off + 2 < out_len) out[off++] = ' ';
    }
    if (off + 2 < out_len) {
        out[off++] = ']';
        out[off] = '\0';
    } else {
        out[out_len - 1] = '\0';
    }
}

static int on_enter(cmd_ui_t* ui, gw_uart_t* uart, int ep, uint8_t dst, uint16_t* seq, int show_packets)
{
    char* p = ui->input;
    while (*p == ' ' || *p == '\t') p++;
    if (strncasecmp(p, "src", 3) == 0 && (p[3] == '\0' || p[3] == ' ' || p[3] == '\t')) {
        uint8_t raw[ECU_MAX_FRAME_SIZE];
        size_t raw_len = 0;
        char err[128];

        if (!parse_src_line(p, raw, &raw_len, err, sizeof(err))) {
            ui_set_status(ui, "ERR: %s", err);
            return 1;
        }

        if (gw_uart_queue_tx(uart, raw, raw_len) < 0) {
            ui_set_status(ui, "ERR: failed to queue RAW TX");
            return 1;
        }
        uart_epoll_refresh(ep, uart);

        if (show_packets) {
            char hex[CMD_UI_LINE_MAX];
            format_hex_preview("TX RAW", raw, raw_len, hex, sizeof(hex));
            ui_add_rx_line(ui, "%s", hex);
        }
        ui_add_rx_line(ui, "TX RAW len=%zu", raw_len);
        ui_set_status(ui, "OK: queued RAW len=%zu", raw_len);
        return 1;
    }

    uint16_t cmd_id = 0;
    uint8_t params[ECU_MAX_PAYLOAD];
    size_t param_len = 0;
    char err[128];

    if (!parse_cmd_line(ui->input, &cmd_id, params, &param_len, err, sizeof(err))) {
        ui_set_status(ui, "ERR: %s", err);
        return 1;
    }

    uint8_t frame[CMD_UI_FRAME_MAX];
    size_t frame_len = 0;
    if (!build_command_frame(*seq, dst, cmd_id, params, param_len, frame, sizeof(frame), &frame_len)) {
        ui_set_status(ui, "ERR: failed to build COMMAND frame");
        return 1;
    }

    if (gw_uart_send_slip(uart, frame, frame_len) < 0) {
        ui_set_status(ui, "ERR: failed to queue SLIP TX");
        return 1;
    }
    uart_epoll_refresh(ep, uart);

    if (show_packets) {
        char hex[CMD_UI_LINE_MAX];
        format_hex_preview("TX", frame, frame_len, hex, sizeof(hex));
        ui_add_rx_line(ui, "%s", hex);
    }

    ui_add_rx_line(ui, "TX COMMAND id=%u seq=%u param_len=%zu", (unsigned)cmd_id, (unsigned)*seq, param_len);
    ui_set_status(ui, "OK: queued command id=%u seq=%u", (unsigned)cmd_id, (unsigned)*seq);
    (*seq)++;
    return 1;
}

static int process_stdin_bytes(cmd_ui_t* ui, const uint8_t* buf, size_t n, gw_uart_t* uart, int ep, uint8_t dst, uint16_t* seq, int show_packets)
{
    int changed = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = buf[i];
        if (c == 3u) {
            g_stop = 1;
            return 1;
        }
        if (c == '\r' || c == '\n') {
            if (ui->input_len > 0) {
                changed |= on_enter(ui, uart, ep, dst, seq, show_packets);
                ui->input_len = 0;
                ui->input[0] = '\0';
                changed = 1;
            } else {
                ui_set_status(ui, "ready");
                changed = 1;
            }
            continue;
        }
        if (c == 127u || c == 8u) {
            if (ui->input_len > 0) {
                ui->input[--ui->input_len] = '\0';
                changed = 1;
            }
            continue;
        }
        if (c == 'q' && ui->input_len == 0) {
            g_stop = 1;
            return 1;
        }
        if (isprint(c)) {
            if (ui->input_len + 1 < sizeof(ui->input)) {
                ui->input[ui->input_len++] = (char)c;
                ui->input[ui->input_len] = '\0';
                changed = 1;
            }
        }
    }
    return changed;
}

static int process_uart_rx(cmd_ui_t* ui, gw_uart_t* uart, int preview_raw, int show_packets)
{
    int changed = 0;
    int decoded_any = 0;
    int decode_error = 0;
    int had_slip_progress = 0;
    int rr = gw_uart_handle_read(uart);
    if (rr < 0) {
        ui_add_rx_line(ui, "ERR: UART read failure");
        return 1;
    }

    uint8_t src_chunk[256];
    size_t src_chunk_len = 0;
    if (rr > 0 && (size_t)rr <= uart->rx_len) {
        const uint8_t* chunk = &uart->rx_buf[uart->rx_len - (size_t)rr];
        src_chunk_len = (size_t)rr;
        if (src_chunk_len > sizeof(src_chunk)) src_chunk_len = sizeof(src_chunk);
        memcpy(src_chunk, chunk, src_chunk_len);
    }

    if (preview_raw && rr > 0 && (size_t)rr <= uart->rx_len) {
        char hex[CMD_UI_LINE_MAX];
        format_hex_preview("RAW", &uart->rx_buf[uart->rx_len - (size_t)rr], (size_t)rr, hex, sizeof(hex));
        ui_add_rx_line(ui, "%s", hex);
        changed = 1;
    }

    for (;;) {
        const uint8_t* frame = NULL;
        size_t frame_len = 0;
        int gr = gw_uart_try_get_slip_frame(uart, &frame, &frame_len);
        if (gr == 0) break;
        if (gr < 0) {
            decode_error = 1;
            char src_line[CMD_UI_LINE_MAX];
            if (src_chunk_len > 0) {
                format_src_bytes(src_chunk, src_chunk_len, src_line, sizeof(src_line));
                ui_add_rx_line(ui, "%s", src_line);
            } else {
                ui_add_rx_line(ui, "SRC: []");
            }
            changed = 1;
            break;
        }

        const ecu_hdr_t* h = NULL;
        const uint8_t* payload = NULL;
        if (!validate_ecu_bytes(frame, frame_len, &h, &payload)) {
            ui_add_rx_line(ui, "DROP bad ECU frame len=%zu", frame_len);
            changed = 1;
            continue;
        }
        decoded_any = 1;

        ui_add_rx_line(ui, "RX msg=0x%02X seq=%u flags=0x%04X len=%u",
                       (unsigned)h->msg_type, (unsigned)h->seq, (unsigned)h->flags, (unsigned)h->payload_len);
        changed = 1;

        if (h->msg_type == ECU_MSG_ACK && h->payload_len >= sizeof(ecu_ack_v1_t)) {
            ecu_ack_v1_t ack;
            memcpy(&ack, payload, sizeof(ack));
            ui_add_rx_line(ui, "ACK ack_seq=%u status=%u", (unsigned)ack.ack_seq, (unsigned)ack.status_code);
            changed = 1;
        } else if (show_packets) {
            char hex[CMD_UI_LINE_MAX];
            format_hex_preview("RX", frame, frame_len, hex, sizeof(hex));
            ui_add_rx_line(ui, "%s", hex);
            changed = 1;
        }
    }

    // If decoder is currently inside a frame or waiting ESC continuation,
    // treat input as partial SLIP and avoid SRC noise.
    if (uart->slip.in_frame || uart->slip.out_len > 0 || uart->slip.esc) {
        had_slip_progress = 1;
    }

    if (rr > 0 && !decoded_any && !decode_error && !had_slip_progress) {
        char src_line[CMD_UI_LINE_MAX];
        if (src_chunk_len > 0) {
            format_src_bytes(src_chunk, src_chunk_len, src_line, sizeof(src_line));
            ui_add_rx_line(ui, "%s", src_line);
        } else {
            ui_add_rx_line(ui, "SRC: []");
        }
        changed = 1;
    }

    return changed;
}

int gw_cmd_ui_run(const char* port_name, int show_packets, int preview_raw)
{
    const char* dev = NULL;
    uint8_t dst = ECU_NODE1;
    if (!port_to_uart(port_name, &dev, &dst)) {
        fprintf(stderr, "Invalid -cmd_ui PORT: %s (use ttyS1|ttyS4|ttyS5)\n", port_name ? port_name : "");
        return 2;
    }

    gw_uart_t uart;
    if (gw_uart_open(&uart, dev, 115200) < 0) {
        perror(dev);
        return 1;
    }

    term_guard_t tg;
    if (set_stdin_raw(&tg) < 0) {
        fprintf(stderr, "cmd_ui requires interactive TTY\n");
        gw_uart_close(&uart);
        return 1;
    }

    struct sigaction old_sa;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    (void)sigaction(SIGINT, &sa, &old_sa);

    int ep = epoll_create1(0);
    if (ep < 0) {
        perror("epoll_create1");
        (void)sigaction(SIGINT, &old_sa, NULL);
        restore_stdin(&tg);
        gw_uart_close(&uart);
        return 1;
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = STDIN_FILENO;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, STDIN_FILENO, &ev) < 0) {
        perror("epoll add stdin");
        close(ep);
        (void)sigaction(SIGINT, &old_sa, NULL);
        restore_stdin(&tg);
        gw_uart_close(&uart);
        return 1;
    }

    memset(&ev, 0, sizeof(ev));
    ev.events = uart_events_mask(&uart);
    ev.data.fd = gw_uart_fd(&uart);
    if (epoll_ctl(ep, EPOLL_CTL_ADD, gw_uart_fd(&uart), &ev) < 0) {
        perror("epoll add uart");
        close(ep);
        (void)sigaction(SIGINT, &old_sa, NULL);
        restore_stdin(&tg);
        gw_uart_close(&uart);
        return 1;
    }

    cmd_ui_t ui;
    memset(&ui, 0, sizeof(ui));
    ui.port_name = port_name;
    ui_set_status(&ui, "ready");

    uint16_t seq = 1;
    g_stop = 0;
    printf("\x1b[?25l");
    ui_redraw(&ui);

    while (!g_stop) {
        int dirty = 0;
        struct epoll_event evs[8];
        int n = epoll_wait(ep, evs, 8, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            ui_add_rx_line(&ui, "epoll_wait error");
            dirty = 1;
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = evs[i].data.fd;
            uint32_t e = evs[i].events;

            if (fd == STDIN_FILENO && (e & EPOLLIN)) {
                uint8_t in[128];
                for (;;) {
                    ssize_t r = read(STDIN_FILENO, in, sizeof(in));
                    if (r < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        ui_set_status(&ui, "ERR: stdin read");
                        g_stop = 1;
                        break;
                    }
                    if (r == 0) break;
                    dirty |= process_stdin_bytes(&ui, in, (size_t)r, &uart, ep, dst, &seq, show_packets);
                    if ((size_t)r < sizeof(in)) break;
                }
            }

            if (fd == gw_uart_fd(&uart)) {
                if (e & EPOLLIN) dirty |= process_uart_rx(&ui, &uart, preview_raw, show_packets);
                if (e & EPOLLOUT) {
                    int wr = gw_uart_handle_write(&uart);
                    if (wr < 0) {
                        ui_add_rx_line(&ui, "ERR: UART write failure");
                        dirty = 1;
                    }
                }
                uart_epoll_refresh(ep, &uart);
            }
        }

        if (dirty) ui_redraw(&ui);
    }

    close(ep);
    (void)sigaction(SIGINT, &old_sa, NULL);
    restore_stdin(&tg);
    gw_uart_close(&uart);
    printf("\x1b[2J\x1b[H\x1b[?25h");
    fflush(stdout);
    return 0;
}
