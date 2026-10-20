#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/event.h>
#include <unistd.h>

#define PORT 6500
#define MAX_EVENTS 64

typedef struct kevent kevent_t;
typedef struct sockaddr_in sockaddr_in_t;

static void handle_data_read(const int fd) {
    char buffer[1024];
    const ssize_t bytes = read(fd, &buffer, sizeof(buffer));

    if (bytes <= 0) {
        printf("Failed to read from socket\n");
        close(fd);
    } else {
        buffer[bytes] = '\0';
        const char *prefix = "From server: ";
        char response[1100];
        strcpy(response, prefix);
        strcat(response, buffer);
        write(fd, response, strlen(response));
    }
}

int main(void) {
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
                    printf("Failed to accept connection from client. Exiting.\n");
                    continue;
                }

                // register this client fd with queue for reading
                kevent_t client_event_config;
                EV_SET(&client_event_config, client_fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
                if (kevent(queue_fd, &client_event_config, 1, NULL, 0, NULL) == -1) {
                    printf("Failed to register event for client. Exiting.\n");
                    continue;
                }

                printf("Accepted connection from client (fd: %d).\n", client_fd);
            } else {
                // since client fd is different from server fd, this means this connection has already
                // been established and now this change event is basically new data coming onto that connection
                if (change_event->flags & EV_EOF) {
                    printf("Client on fd: %d disconnected. Exiting.\n", event_fd);
                    close(event_fd);
                } else if (change_event->filter == EVFILT_READ) {
                    handle_data_read(event_fd);
                }
            }
        }
    }

    close(server_fd);
    close(queue_fd);
    return 0;
}
