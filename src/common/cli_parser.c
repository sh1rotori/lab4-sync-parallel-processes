#include "cli_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void cli_config_init_defaults(cli_config_t *config) {
    if (!config) return;
    config->threads = 4;
    config->buffer_size = 16;
    config->items = 100000;
    config->no_sync = false;
    config->verbose = false;
}

void cli_print_usage(const char *prog_name) {
    printf("Usage: %s [options]\n", prog_name);
    printf("Options:\n");
    printf("  -t, --threads <N>   Number of worker threads (default: 4)\n");
    printf("  -b, --buffer <N>    Circular buffer capacity (default: 16)\n");
    printf("  -i, --items <N>     Number of items/iterations (default: 100000)\n");
    printf("  -n, --no-sync       Disable synchronization (demonstrates race conditions)\n");
    printf("  -v, --verbose       Enable verbose output\n");
    printf("  -h, --help          Show this help message and exit\n");
}

int cli_parse_args(int argc, char **argv, cli_config_t *config, const char *prog_name) {
    if (!config) return -1;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--threads") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: '%s' requires a value.\n", argv[i]);
                cli_print_usage(prog_name);
                return -1;
            }
            config->threads = atoi(argv[++i]);
            if (config->threads <= 0) {
                fprintf(stderr, "Error: threads count must be > 0.\n");
                return -1;
            }
        } else if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--buffer") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: '%s' requires a value.\n", argv[i]);
                cli_print_usage(prog_name);
                return -1;
            }
            long val = atol(argv[++i]);
            if (val <= 0) {
                fprintf(stderr, "Error: buffer size must be > 0.\n");
                return -1;
            }
            config->buffer_size = (size_t)val;
        } else if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--items") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: '%s' requires a value.\n", argv[i]);
                cli_print_usage(prog_name);
                return -1;
            }
            long val = atol(argv[++i]);
            if (val <= 0) {
                fprintf(stderr, "Error: items count must be > 0.\n");
                return -1;
            }
            config->items = (size_t)val;
        } else if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--no-sync") == 0) {
            config->no_sync = true;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            config->verbose = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            cli_print_usage(prog_name);
            return 1;
        } else {
            fprintf(stderr, "Unknown option: '%s'\n", argv[i]);
            cli_print_usage(prog_name);
            return -1;
        }
    }

    return 0;
}
