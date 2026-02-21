#include "gw/gw_app.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char** argv)
{
    int show_packets = 0;
    int preview_raw = 0;
    const char* send_test_ports = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-show") == 0) {
            show_packets = 1;
            continue;
        }
        if (strcmp(argv[i], "-prev_show") == 0) {
            preview_raw = 1;
            continue;
        }
        if (strcmp(argv[i], "-send_test") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Usage: %s [-show] [-prev_show] [-send_test PORT]\n", argv[0]);
                return 2;
            }
            send_test_ports = argv[++i];
            continue;
        }

        fprintf(stderr, "Usage: %s [-show] [-prev_show] [-send_test PORT]\n", argv[0]);
        return 2;
    }

    return gw_app_run(show_packets, preview_raw, send_test_ports);
}
