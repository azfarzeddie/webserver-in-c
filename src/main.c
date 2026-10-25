#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/event.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>
#include <errno.h>
#include <signal.h>
#include "http_parser.h"

#define PORT 6500
#define MAX_EVENTS 64

typedef struct kevent kevent_t;
typedef struct sockaddr_in sockaddr_in_t;

static void close_connection(connection_t *connection) {
    close(connection->socket_fd);
    free(connection);
}

static const char *status_text(const int status) {
    switch (status) {
        case 414: return "URI Too Long";
        case 431: return "Request Header Fields Too Large";
        default: return "Bad Request";
    }
}

static void send_error(connection_t *connection, const int status) {
    char response[256];
    const int length = snprintf(response, sizeof(response),
                                "HTTP/1.1 %d %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
                                status, status_text(status));
    if (length > 0) {
        write(connection->socket_fd, response, (size_t) length);
    }
    close_connection(connection);
}

static void handle_data_read(connection_t *connection) {
    if (connection->state == STATE_ERROR) {
        send_error(connection, connection->error_status ? connection->error_status : 400);
        return;
    }
    if (connection->state == STATE_DONE) {
        return;
    }

    // read the data from the socket to the read buffer of connection object and then call parse
    const size_t space = sizeof(connection->read_buffer) - connection->buffer_length;
    if (space == 0) {
        send_error(connection, 431);
        return;
    }
    const ssize_t bytes = read(connection->socket_fd,
                               connection->read_buffer + connection->buffer_length,
                               space);
    if (bytes == 0) {
        // nothing to read
        close_connection(connection);
        return;
    }
    if (bytes < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            printf("Data not available to be read yet or interrupted. Returning back to event loop.\n");
        } else {
            printf("Failed to read from socket\n");
            close_connection(connection);
        }
        return;
    }
    connection->buffer_length += (size_t) bytes;
    parse(connection);
    if (connection->state == STATE_ERROR) {
        send_error(connection, connection->error_status ? connection->error_status : 400);
    }
}

static bool set_nonblocking(const int client_fd) {
    const int flags = fcntl(client_fd, F_GETFL);
    if (flags == -1 || fcntl(client_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        printf("Failed to set nonblocking on client.\n");
        return false;
    }
    return true;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    printf("Starting up the server...\n");
    // init a socket and get the file descriptor assigned
    const int server_fd = socket(PF_INET, SOCK_STREAM, 0);
    // set options so that upon immediate restart, same port is available to bind
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));

    // socket is acquired but is not bound to any IP and port, bind does that
    // create a socketaddr_in type (defined in netinet/in.h) object which
    // specifies the IP port details
    sockaddr_in_t address = {
        .sin_family = PF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(PORT)
    };
    const int bind_res = bind(server_fd, (struct sockaddr *) &address, sizeof(address));
    if (bind_res == -1) {
        printf("Failed to bind the socket to port. Exiting.\n");
        exit(1);
    }

    // listen for new connections on the socket
    listen(server_fd, 128);
    if (!set_nonblocking(server_fd)) {
        printf("Failed to set nonblocking on server. Exiting.\n");
        close(server_fd);
        exit(1);
    }

    // create a queue to add incoming connections
    const int queue_fd = kqueue();
    if (queue_fd == -1) {
        printf("Failed to create event queue for port: %d\n", PORT);
        exit(1);
    }
    // configure a kevent object which tells under what condition the socket should notify about an event
    kevent_t change_event_config;
    EV_SET(&change_event_config, server_fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
    // now register the event queue with these settings
    if (kevent(queue_fd, &change_event_config, 1, NULL, 0, NULL) == -1) {
        printf("Failed to register kevent with event queue. Exiting.\n");
        exit(1);
    }
    printf("Listening on port %d...\n", PORT);

    kevent_t event_list[MAX_EVENTS];
    while (1) {
        const int num_events = kevent(queue_fd, NULL, 0, event_list, MAX_EVENTS, NULL);
        if (num_events == -1) {
            if (errno == EINTR) continue;
            printf("Failed to get the number of events. Exiting.\n");
            exit(1);
        }

        for (int i = 0; i < num_events; i++) {
            // handle the events here
            const kevent_t *change_event = &event_list[i];
            const int event_fd = (int) change_event->ident;
            if (event_fd == server_fd) {
                // if the identifier of an event is the same as the socket fd, then it means this is a new connection
                // we need to accept this connection and then create a new file descriptor for it and then
                // register that file descriptor with kevent so that when data comes through this connection
                // the event loop can notify us
                sockaddr_in_t client_address;
                socklen_t client_address_size = sizeof(client_address);
                const int client_fd = accept(server_fd, (struct sockaddr *) &client_address, &client_address_size);
                if (client_fd == -1) {
                    if (errno == ECONNABORTED || errno == EAGAIN || errno == EWOULDBLOCK) {
                        continue;
                    }
                    printf("Failed to accept connection from client. Continuing.\n");
                    continue;
                }

                // in an event driven architecture, a single slow read or write
                // could block the entire event loop, so set socket as non-blocking
                if (!set_nonblocking(client_fd)) {
                    close(client_fd);
                    continue;
                }

                connection_t *connection = calloc(1, sizeof(connection_t));
                if (connection == NULL) {
                    printf("Failed to allocate memory for connection. Dropping client.\n");
                    close(client_fd);
                    continue;
                }
                connection->socket_fd = client_fd;
                connection->state = STATE_METHOD;
                // register this client fd with queue for reading
                kevent_t client_event_config;
                EV_SET(&client_event_config, client_fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, connection);
                if (kevent(queue_fd, &client_event_config, 1, NULL, 0, NULL) == -1) {
                    printf("Failed to register event for client.\n");
                    close_connection(connection);
                    continue;
                }

                printf("Accepted connection from client (fd: %d).\n", client_fd);
            } else {
                // since client fd is different from server fd, this means this connection has already
                // been established and now this change event is basically new data coming onto that connection
                if (change_event->flags & EV_EOF) {
                    printf("Client on fd: %d disconnected.\n", event_fd);
                    close_connection(change_event->udata);
                } else if (change_event->filter == EVFILT_READ) {
                    handle_data_read(change_event->udata);
                }
            }
        }
    }

    close(server_fd);
    close(queue_fd);
    return 0;
}
