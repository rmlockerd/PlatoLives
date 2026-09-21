#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plato/console_runner.h"

int main(int argc, char *argv[]) {
    const char *host = NULL;
    int port = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--v") == 0 || strcmp(argv[i], "-V") == 0) {
            printf("PlatoLives v3.8\n");
            return 0;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("PlatoLives Console Terminal (Clean-room PLATO client)\n");
            printf("Usage: %s [host] [port]\n", argv[0]);
            printf("Default: cyberserv.org:8005\n");
            return 0;
        }
        if (argv[i][0] != '-') {
            if (!host) {
                host = argv[i];
            } else if (port == 0) {
                port = atoi(argv[i]);
            }
        }
    }

    return plato_console_run(host, port);
}
