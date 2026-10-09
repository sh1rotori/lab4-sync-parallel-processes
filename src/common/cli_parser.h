#ifndef CLI_PARSER_H
#define CLI_PARSER_H

#include <stdbool.h>
#include <stddef.h>

/**
 * Command-line argument configuration for benchmark modules:
 *   --threads <num>  : Number of worker threads
 *   --buffer <num>   : Circular buffer capacity
 *   --items <num>    : Number of operations / items per thread
 *   --no-sync        : Disable synchronization to demonstrate race conditions
 *   --verbose        : Enable verbose logging
 */
typedef struct {
    int threads;         /* Number of worker threads (default: 4) */
    size_t buffer_size;  /* Buffer capacity (default: 16) */
    size_t items;        /* Items / iterations count (default: 100000) */
    bool no_sync;        /* Disable synchronization flag */
    bool verbose;        /* Verbose output flag */
} cli_config_t;

void cli_config_init_defaults(cli_config_t *config);
int cli_parse_args(int argc, char **argv, cli_config_t *config, const char *prog_name);
void cli_print_usage(const char *prog_name);

#endif /* CLI_PARSER_H */
