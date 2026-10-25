#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

#include <stddef.h>

typedef enum parser_state_t {
    STATE_METHOD,
    STATE_URI,
    STATE_VERSION,
    STATE_HEADER_KEY,
    STATE_HEADER_VALUE,
    STATE_BODY,
    STATE_DONE,
    STATE_ERROR
} parser_state_t;

typedef struct header_t {
    char key[64];
    char value[256];
} header_t;

typedef struct connection_t {
    int socket_fd;
    parser_state_t state;
    char read_buffer[4096];
    size_t buffer_length;
    size_t parse_offset;

    char method[8];
    char version[16];
    char uri[1024];
    header_t headers[32];
    int header_count;
    char *body;

    size_t content_length;
    size_t body_bytes_read;
} connection_t;

#endif
