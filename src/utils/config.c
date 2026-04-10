#include <stdlib.h>
#include <getopt.h>
#include <string.h>

#include "wsland/utils/log.h"
#include "wsland/utils/config.h"

wsland_config* wsland_config_create(int argc, char *argv[]) {
    wsland_config *config = calloc(1, sizeof(*config));
    if (!config) {
        wsland_log(CONFIG, ERROR, "%s", "calloc failed for wsland_config");
        goto create_failed;
    }

    { // parse args
        int c;
        while ((c = getopt(argc, argv, "s:h")) != -1) {
            switch (c) {
            case 's':
                config->command = optarg;
                break;
            default:
                wsland_log(CONFIG, INFO, "usage: %s [-s command]", argv[0]);
                goto create_failed;
            }
        }

        if (optind < argc) {
            wsland_log(CONFIG, ERROR, "usage: %s [-s command]", argv[0]);
            goto create_failed;
        }
    }

    { // parse envs
        config->address = strdup("0.0.0.0");
        config->port = 3389;
        config->socket_name = strdup("wayland-0");

        const char *address = getenv("WSLAND_ADDR");
        if (address) {
            free(config->address);
            config->address = strdup(address);
        }

        const char *temp_port = getenv("WSLAND_PORT");
        if (temp_port) {
            char *end_ptr;
            int port = (int)strtol(temp_port, &end_ptr, 10);
            if (*end_ptr || port <= 0 || port > 65535) {
                wsland_log(CONFIG, ERROR, "%s", "expected env [ WSLAND_PORT ] to be a positive integer less or equal to 65535");
                goto create_failed;
            }
            config->port = port;
        }

        const char *socket_name = getenv("WAYLAND_DISPLAY");
        if (socket_name && strlen(socket_name) != 0) {
            free(config->socket_name);
            config->socket_name = strdup(socket_name);
        }

        const char *notify_socket = getenv("WSLGD_NOTIFY_SOCKET");
        if (notify_socket && strlen(notify_socket) != 0) {
            config->notify_socket = strdup(notify_socket);
        }
    }

    return config;
create_failed:
    wsland_config_destroy(config);
    return NULL;
}

void wsland_config_destroy(wsland_config *config) {
    if (config) {
        free(config->notify_socket);
        free(config->socket_name);
        free(config->address);
        free(config);
    }
}
