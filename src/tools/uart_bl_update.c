#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define BL_FRAME_MAGIC 0xB10Cu
#define BL_FRAME_VERSION 1u

#define BL_CMD_INFO 0x01u
#define BL_CMD_ERASE 0x02u
#define BL_CMD_WRITE 0x03u
#define BL_CMD_VERIFY 0x04u
#define BL_CMD_RUN 0x05u

#define BL_ACK 0x79u

#define ECU_MAGIC 0xEC10u
#define ECU_VERSION 1u
#define ECU_MSG_COMMAND 0x03u
#define ECU_SRC_T113 0xFFu
#define ECU_DST_NODE1 0x01u
#define ECU_CMD_ENTER_BOOT 8u

#define SYNC_REPLY "BL>OK\n"
#define N_RETRY 3u

typedef struct {
    uint8_t code;
    uint8_t status;
    uint16_t payload_len;
    uint16_t seq;
    uint32_t detail;
    uint8_t *payload;
} bl_resp_t;

typedef struct {
    const char *port;
    int baud;
    const char *firmware;
    uint32_t chunk;
    uint32_t boot_wait_ms;
    bool no_enter_boot;
    bool no_run;
} args_t;

static void wr_u16_le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void wr_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint16_t rd_u16_le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000u) + ((uint64_t)ts.tv_nsec / 1000000u);
}

static uint32_t crc32_calc(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint32_t b = 0; b < 8u; b++) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

static uint16_t crc16_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t b = 0; b < 8u; b++) {
            if ((crc & 0x8000u) != 0u) {
                crc = (uint16_t)((crc << 1) ^ 0x1021u);
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

static int write_all(int fd, const uint8_t *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static int read_exact_timeout(int fd, uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    size_t off = 0;
    uint64_t deadline = now_ms() + timeout_ms;
    while (off < len) {
        uint64_t now = now_ms();
        if (now >= deadline) {
            return -1;
        }

        uint32_t wait_ms = (uint32_t)(deadline - now);
        struct timeval tv;
        tv.tv_sec = (time_t)(wait_ms / 1000u);
        tv.tv_usec = (suseconds_t)((wait_ms % 1000u) * 1000u);

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        int rc = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (rc == 0) {
            return -1;
        }

        ssize_t n = read(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static int set_serial_raw(int fd, int baud)
{
    struct termios tio;
    if (tcgetattr(fd, &tio) != 0) {
        return -1;
    }

    cfmakeraw(&tio);
    tio.c_cflag &= ~PARENB;
    tio.c_cflag &= ~CSTOPB;
    tio.c_cflag &= ~CSIZE;
    tio.c_cflag |= CS8;
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~CRTSCTS;
    tio.c_iflag &= ~(IXON | IXOFF | IXANY);

    speed_t spd = B115200;
    switch (baud) {
        case 115200: spd = B115200; break;
        case 230400: spd = B230400; break;
#ifdef B460800
        case 460800: spd = B460800; break;
#endif
#ifdef B921600
        case 921600: spd = B921600; break;
#endif
        default:
            return -1;
    }

    if (cfsetispeed(&tio, spd) != 0 || cfsetospeed(&tio, spd) != 0) {
        return -1;
    }
    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        return -1;
    }
    tcflush(fd, TCIOFLUSH);
    return 0;
}

static int open_serial(const char *port, int baud)
{
    int fd = open(port, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        return -1;
    }
    if (set_serial_raw(fd, baud) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int send_enter_boot(int fd)
{
    uint8_t payload[4];
    uint8_t frame[16 + sizeof(payload) + 2];
    uint8_t slip[2 * sizeof(frame) + 2];
    size_t out = 0;

    wr_u16_le(payload + 0, ECU_CMD_ENTER_BOOT);
    wr_u16_le(payload + 2, 0u);

    wr_u16_le(frame + 0, ECU_MAGIC);
    frame[2] = ECU_VERSION;
    frame[3] = ECU_MSG_COMMAND;
    frame[4] = ECU_SRC_T113;
    frame[5] = ECU_DST_NODE1;
    wr_u16_le(frame + 6, 1u);
    wr_u16_le(frame + 8, 0u);
    wr_u16_le(frame + 10, sizeof(payload));
    wr_u16_le(frame + 12, 0u);
    wr_u16_le(frame + 14, 0u);
    memcpy(frame + 16, payload, sizeof(payload));
    wr_u16_le(frame + 20, crc16_ccitt(frame, 20));

    slip[out++] = 0xC0u;
    for (size_t i = 0; i < sizeof(frame); i++) {
        uint8_t b = frame[i];
        if (b == 0xC0u) {
            slip[out++] = 0xDBu;
            slip[out++] = 0xDCu;
        } else if (b == 0xDBu) {
            slip[out++] = 0xDBu;
            slip[out++] = 0xDDu;
        } else {
            slip[out++] = b;
        }
    }
    slip[out++] = 0xC0u;

    return write_all(fd, slip, out);
}

static int wait_sync_ok(int fd, uint32_t boot_wait_ms)
{
    const uint8_t sync[4] = {0x55u, 0xAAu, 0x55u, 0xAAu};
    const char *needle = SYNC_REPLY;
    const size_t needle_len = strlen(needle);
    uint8_t buf[64];
    size_t fill = 0;
    uint64_t deadline = now_ms() + boot_wait_ms;

    while (now_ms() < deadline) {
        if (write_all(fd, sync, sizeof(sync)) != 0) {
            return -1;
        }

        uint64_t step_deadline = now_ms() + 500u;
        while (now_ms() < step_deadline) {
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 100000;
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(fd, &rfds);
            int rc = select(fd + 1, &rfds, NULL, NULL, &tv);
            if (rc <= 0) {
                continue;
            }
            uint8_t b = 0;
            ssize_t n = read(fd, &b, 1);
            if (n <= 0) {
                continue;
            }
            if (fill < sizeof(buf)) {
                buf[fill++] = b;
            } else {
                memmove(buf, buf + 1, sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = b;
            }

            if (fill >= needle_len) {
                for (size_t i = 0; i + needle_len <= fill; i++) {
                    if (memcmp(buf + i, needle, needle_len) == 0) {
                        return 0;
                    }
                }
            }
        }
    }
    return -1;
}

static int ping_sync_once(int fd)
{
    const uint8_t sync[4] = {0x55u, 0xAAu, 0x55u, 0xAAu};
    uint8_t rx[6];
    if (write_all(fd, sync, sizeof(sync)) != 0) {
        return -1;
    }
    if (read_exact_timeout(fd, rx, sizeof(rx), 700u) != 0) {
        return -1;
    }
    if (memcmp(rx, SYNC_REPLY, sizeof(rx)) == 0) {
        return 0;
    }
    return -1;
}

static int send_bl_cmd(int fd, uint8_t cmd, uint16_t seq, const uint8_t *payload, uint16_t payload_len)
{
    size_t frame_len = 8u + payload_len + 4u;
    uint8_t *frame = (uint8_t *)malloc(frame_len);
    if (frame == NULL) {
        return -1;
    }

    wr_u16_le(frame + 0, BL_FRAME_MAGIC);
    frame[2] = BL_FRAME_VERSION;
    frame[3] = cmd;
    wr_u16_le(frame + 4, payload_len);
    wr_u16_le(frame + 6, seq);
    if (payload_len > 0u) {
        memcpy(frame + 8, payload, payload_len);
    }
    wr_u32_le(frame + 8 + payload_len, crc32_calc(frame, 8u + payload_len));

    int rc = write_all(fd, frame, frame_len);
    free(frame);
    return rc;
}

static int read_bl_resp(int fd, bl_resp_t *resp, uint32_t timeout_ms)
{
    uint8_t hdr[14];
    if (read_exact_timeout(fd, hdr, sizeof(hdr), timeout_ms) != 0) {
        return -1;
    }

    resp->code = hdr[0];
    resp->status = hdr[1];
    resp->payload_len = rd_u16_le(hdr + 2);
    resp->seq = rd_u16_le(hdr + 4);
    resp->detail = rd_u32_le(hdr + 6);
    uint32_t rx_crc = rd_u32_le(hdr + 10);

    resp->payload = NULL;
    if (resp->payload_len > 0u) {
        resp->payload = (uint8_t *)malloc(resp->payload_len);
        if (resp->payload == NULL) {
            return -1;
        }
        if (read_exact_timeout(fd, resp->payload, resp->payload_len, timeout_ms) != 0) {
            free(resp->payload);
            resp->payload = NULL;
            return -1;
        }
    }

    uint8_t *crc_buf = (uint8_t *)malloc(10u + resp->payload_len);
    if (crc_buf == NULL) {
        free(resp->payload);
        resp->payload = NULL;
        return -1;
    }
    memcpy(crc_buf, hdr, 10u);
    if (resp->payload_len > 0u) {
        memcpy(crc_buf + 10u, resp->payload, resp->payload_len);
    }
    uint32_t calc_crc = crc32_calc(crc_buf, 10u + resp->payload_len);
    free(crc_buf);

    if (calc_crc != rx_crc) {
        free(resp->payload);
        resp->payload = NULL;
        return -1;
    }
    return 0;
}

static void free_bl_resp(bl_resp_t *resp)
{
    if (resp->payload != NULL) {
        free(resp->payload);
        resp->payload = NULL;
    }
}

static int bl_xfer(int fd, uint8_t cmd, uint16_t seq, const uint8_t *payload, uint16_t payload_len,
                   uint32_t timeout_ms, bl_resp_t *resp)
{
    if (send_bl_cmd(fd, cmd, seq, payload, payload_len) != 0) {
        return -1;
    }
    if (read_bl_resp(fd, resp, timeout_ms) != 0) {
        return -1;
    }
    if (resp->seq != seq) {
        return -1;
    }
    if (resp->code != BL_ACK) {
        return -2;
    }
    return 0;
}

static int load_file(const char *path, uint8_t **out_data, size_t *out_len)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }
    if (st.st_size <= 0) {
        return -1;
    }

    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return -1;
    }
    uint8_t *data = (uint8_t *)malloc((size_t)st.st_size);
    if (data == NULL) {
        fclose(fp);
        return -1;
    }
    size_t n = fread(data, 1, (size_t)st.st_size, fp);
    fclose(fp);
    if (n != (size_t)st.st_size) {
        free(data);
        return -1;
    }
    *out_data = data;
    *out_len = n;
    return 0;
}

static void print_usage(const char *argv0)
{
    printf("Usage: %s --port /dev/ttyS1 [options]\n", argv0);
    printf("Options:\n");
    printf("  --baud <n>           (default 115200)\n");
    printf("  --firmware <path>    app image file\n");
    printf("  --chunk <n>          write chunk size (default 1024)\n");
    printf("  --boot-wait-ms <n>   wait BL after ENTER_BOOT (default 5000)\n");
    printf("  --no-enter-boot      do not send ENTER_BOOT command\n");
    printf("  --no-run             do not send CMD_RUN after verify\n");
}

static int parse_args(int argc, char **argv, args_t *a)
{
    memset(a, 0, sizeof(*a));
    a->baud = 115200;
    a->chunk = 1024u;
    a->boot_wait_ms = 5000u;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            a->port = argv[++i];
        } else if (strcmp(argv[i], "--baud") == 0 && i + 1 < argc) {
            a->baud = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--firmware") == 0 && i + 1 < argc) {
            a->firmware = argv[++i];
        } else if (strcmp(argv[i], "--chunk") == 0 && i + 1 < argc) {
            a->chunk = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--boot-wait-ms") == 0 && i + 1 < argc) {
            a->boot_wait_ms = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--no-enter-boot") == 0) {
            a->no_enter_boot = true;
        } else if (strcmp(argv[i], "--no-run") == 0) {
            a->no_run = true;
        } else {
            return -1;
        }
    }

    if (a->port == NULL) {
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    args_t a;
    if (parse_args(argc, argv, &a) != 0) {
        print_usage(argv[0]);
        return 1;
    }

    int fd = open_serial(a.port, a.baud);
    if (fd < 0) {
        perror("open_serial");
        return 1;
    }

    if (!a.no_enter_boot) {
        if (send_enter_boot(fd) != 0) {
            perror("send_enter_boot");
            close(fd);
            return 1;
        }
        usleep(200000);
    }

    if (wait_sync_ok(fd, a.boot_wait_ms) != 0) {
        fprintf(stderr, "ERROR: no BL sync reply\n");
        close(fd);
        return 1;
    }
    printf("SYNC: OK\n");

    uint16_t seq = 1u;
    bl_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    if (bl_xfer(fd, BL_CMD_INFO, seq++, NULL, 0u, 1500u, &resp) != 0) {
        fprintf(stderr, "ERROR: CMD_INFO failed\n");
        close(fd);
        return 1;
    }
    if (resp.payload_len != 16u) {
        fprintf(stderr, "ERROR: INFO bad payload len %u\n", resp.payload_len);
        free_bl_resp(&resp);
        close(fd);
        return 1;
    }

    uint32_t bl_ver = rd_u32_le(resp.payload + 0);
    uint32_t app_start = rd_u32_le(resp.payload + 4);
    uint32_t flash_end = rd_u32_le(resp.payload + 8);
    uint16_t page_size = rd_u16_le(resp.payload + 12);
    uint16_t max_chunk = rd_u16_le(resp.payload + 14);
    free_bl_resp(&resp);
    printf("INFO: bl=0x%08" PRIX32 " app_start=0x%08" PRIX32 " flash_end=0x%08" PRIX32 " page=%u max_chunk=%u\n",
           bl_ver, app_start, flash_end, page_size, max_chunk);

    if (a.firmware == NULL) {
        close(fd);
        return 0;
    }

    uint8_t *image = NULL;
    size_t image_len = 0;
    if (load_file(a.firmware, &image, &image_len) != 0) {
        fprintf(stderr, "ERROR: cannot load firmware %s\n", a.firmware);
        close(fd);
        return 1;
    }
    if (image_len > (size_t)(flash_end - app_start + 1u)) {
        fprintf(stderr, "ERROR: firmware too large\n");
        free(image);
        close(fd);
        return 1;
    }

    uint32_t erase_len = ((uint32_t)image_len + page_size - 1u) / page_size * page_size;
    uint8_t erase_pl[8];
    wr_u32_le(erase_pl + 0, app_start);
    wr_u32_le(erase_pl + 4, erase_len);
    memset(&resp, 0, sizeof(resp));
    int rc = bl_xfer(fd, BL_CMD_ERASE, seq++, erase_pl, sizeof(erase_pl), 20000u, &resp);
    if (rc != 0) {
        if (rc == -2) {
            fprintf(stderr, "ERROR: ERASE NAK status=0x%02X detail=0x%08" PRIX32 "\n", resp.status, resp.detail);
            free_bl_resp(&resp);
        } else {
            fprintf(stderr, "ERROR: ERASE failed\n");
        }
        free(image);
        close(fd);
        return 1;
    }
    free_bl_resp(&resp);
    printf("ERASE: OK len=%" PRIu32 "\n", erase_len);

    uint32_t chunk = a.chunk;
    if (chunk == 0u || chunk > max_chunk) {
        chunk = max_chunk;
    }

    for (uint32_t off = 0u; off < (uint32_t)image_len; ) {
        uint16_t len = (uint16_t)(((uint32_t)image_len - off > chunk) ? chunk : ((uint32_t)image_len - off));
        size_t pl_len = 10u + len;
        uint8_t *pl = (uint8_t *)malloc(pl_len);
        if (pl == NULL) {
            fprintf(stderr, "ERROR: oom\n");
            free(image);
            close(fd);
            return 1;
        }
        wr_u32_le(pl + 0, app_start + off);
        wr_u16_le(pl + 4, len);
        wr_u32_le(pl + 6, crc32_calc(image + off, len));
        memcpy(pl + 10, image + off, len);

        bool ok = false;
        for (uint32_t tr = 0u; tr < N_RETRY; tr++) {
            memset(&resp, 0, sizeof(resp));
            rc = bl_xfer(fd, BL_CMD_WRITE, seq, pl, (uint16_t)pl_len, 20000u, &resp);
            if (rc == 0) {
                ok = true;
                free_bl_resp(&resp);
                break;
            }
            if (rc == -2) {
                fprintf(stderr, "WARN: WRITE off=%" PRIu32 " NAK status=0x%02X detail=0x%08" PRIX32 " retry=%" PRIu32 "\n",
                        off, resp.status, resp.detail, tr + 1u);
                free_bl_resp(&resp);
            } else {
                fprintf(stderr, "WARN: WRITE off=%" PRIu32 " timeout/io error retry=%" PRIu32 "\n", off, tr + 1u);
                if (ping_sync_once(fd) == 0) {
                    fprintf(stderr, "WARN: BL alive after write timeout\n");
                } else {
                    fprintf(stderr, "WARN: BL no sync reply after write timeout\n");
                }
            }
        }
        free(pl);
        if (!ok) {
            fprintf(stderr, "ERROR: WRITE failed at off=%" PRIu32 "\n", off);
            free(image);
            close(fd);
            return 1;
        }
        printf("WRITE: %" PRIu32 "/%zu\n", off + len, image_len);
        off += len;
        seq++;
    }

    uint8_t verify_pl[12];
    wr_u32_le(verify_pl + 0, app_start);
    wr_u32_le(verify_pl + 4, (uint32_t)image_len);
    wr_u32_le(verify_pl + 8, crc32_calc(image, image_len));
    free(image);
    memset(&resp, 0, sizeof(resp));
    rc = bl_xfer(fd, BL_CMD_VERIFY, seq++, verify_pl, sizeof(verify_pl), 5000u, &resp);
    if (rc != 0) {
        if (rc == -2) {
            fprintf(stderr, "ERROR: VERIFY NAK status=0x%02X detail=0x%08" PRIX32 "\n", resp.status, resp.detail);
            free_bl_resp(&resp);
        } else {
            fprintf(stderr, "ERROR: VERIFY failed\n");
        }
        close(fd);
        return 1;
    }
    free_bl_resp(&resp);
    printf("VERIFY: OK\n");

    if (!a.no_run) {
        memset(&resp, 0, sizeof(resp));
        rc = bl_xfer(fd, BL_CMD_RUN, seq++, NULL, 0u, 1500u, &resp);
        if (rc != 0) {
            if (rc == -2) {
                fprintf(stderr, "ERROR: RUN NAK status=0x%02X detail=0x%08" PRIX32 "\n", resp.status, resp.detail);
                free_bl_resp(&resp);
            } else {
                fprintf(stderr, "ERROR: RUN failed\n");
            }
            close(fd);
            return 1;
        }
        free_bl_resp(&resp);
        printf("RUN: OK\n");
    }

    close(fd);
    return 0;
}
