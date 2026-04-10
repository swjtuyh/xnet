/*
 * epoll_server.c - Non-blocking, level-triggered epoll TCP server demo
 *
 * Features:
 *   - Non-blocking sockets (O_NONBLOCK)
 *   - Level-triggered epoll (default, no EPOLLET)
 *   - SO_REUSEADDR / SO_REUSEPORT to avoid "Address already in use"
 *   - Graceful shutdown on SIGINT / SIGTERM
 *   - Echo protocol: reflects every received message back to the client
 *   - Robust error handling throughout
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>

#define DEFAULT_PORT     9000
#define MAX_EVENTS       64
#define RECV_BUF_SIZE    4096
#define BACKLOG          128

/* Set to 1 by signal handler to request a clean exit */
static volatile sig_atomic_t g_stop = 0;

static void signal_handler(int signo)
{
    (void)signo;
    g_stop = 1;
}

/* Make a file descriptor non-blocking. Returns 0 on success, -1 on error. */
static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl F_GETFL");
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl F_SETFL O_NONBLOCK");
        return -1;
    }
    return 0;
}

/* Create, bind and listen on a TCP socket. Returns fd or -1 on error. */
static int create_listen_socket(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        perror("setsockopt SO_REUSEADDR");
        close(fd);
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) == -1) {
        perror("setsockopt SO_REUSEPORT");
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd, BACKLOG) == -1) {
        perror("listen");
        close(fd);
        return -1;
    }

    if (set_nonblocking(fd) == -1) {
        close(fd);
        return -1;
    }

    return fd;
}

/* Register fd with epoll instance epfd for events. Returns 0 or -1. */
static int epoll_add(int epfd, int fd, uint32_t events)
{
    struct epoll_event ev;
    ev.events  = events;
    ev.data.fd = fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) == -1) {
        perror("epoll_ctl EPOLL_CTL_ADD");
        return -1;
    }
    return 0;
}

/* Remove fd from epoll instance epfd. */
static void epoll_del(int epfd, int fd)
{
    if (epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL) == -1) {
        perror("epoll_ctl EPOLL_CTL_DEL");
    }
}

/* Accept all pending connections on listen_fd and register them. */
static void accept_connections(int epfd, int listen_fd)
{
    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int conn_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (conn_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* All pending connections have been accepted */
                break;
            }
            perror("accept");
            break;
        }

        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
        printf("[server] new connection from %s:%d (fd=%d)\n",
               ip_str, ntohs(client_addr.sin_port), conn_fd);

        if (set_nonblocking(conn_fd) == -1) {
            close(conn_fd);
            continue;
        }

        /* Enable TCP keep-alive to detect dead peers */
        int keepalive = 1;
        setsockopt(conn_fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));

        /* Level-triggered: monitor for readable data and errors */
        if (epoll_add(epfd, conn_fd, EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP) == -1) {
            close(conn_fd);
        }
    }
}

/* Echo data from conn_fd back to the peer.  Returns false if connection closed. */
static int handle_client(int epfd, int conn_fd)
{
    char buf[RECV_BUF_SIZE];

    for (;;) {
        ssize_t n = recv(conn_fd, buf, sizeof(buf), 0);
        if (n == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* No more data available right now */
                break;
            }
            perror("recv");
            goto disconnect;
        }
        if (n == 0) {
            /* Peer closed connection */
            printf("[server] client fd=%d disconnected\n", conn_fd);
            goto disconnect;
        }

        /* Echo the data back (handle partial sends) */
        ssize_t sent = 0;
        while (sent < n) {
            ssize_t s = send(conn_fd, buf + sent, (size_t)(n - sent), MSG_NOSIGNAL);
            if (s == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    /*
                     * Send buffer is full.  A production server would buffer
                     * the remaining bytes and register EPOLLOUT, then flush
                     * once the socket is writable again.  Here we yield the
                     * CPU and retry to avoid a busy-wait while keeping the
                     * demo simple.
                     */
                    sched_yield();
                    continue;
                }
                perror("send");
                goto disconnect;
            }
            sent += s;
        }

        printf("[server] echoed %zd byte(s) to fd=%d\n", n, conn_fd);
    }
    return 1; /* connection still alive */

disconnect:
    epoll_del(epfd, conn_fd);
    close(conn_fd);
    return 0;
}

int main(int argc, char *argv[])
{
    uint16_t port = DEFAULT_PORT;
    if (argc == 2) {
        long p = strtol(argv[1], NULL, 10);
        if (p <= 0 || p > 65535) {
            fprintf(stderr, "Usage: %s [port]\n", argv[0]);
            return EXIT_FAILURE;
        }
        port = (uint16_t)p;
    }

    /* Install signal handlers for graceful shutdown */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT,  &sa, NULL) == -1 ||
        sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("sigaction");
        return EXIT_FAILURE;
    }
    /* Ignore SIGPIPE so broken-pipe doesn't kill the server */
    signal(SIGPIPE, SIG_IGN);

    int listen_fd = create_listen_socket(port);
    if (listen_fd == -1) {
        return EXIT_FAILURE;
    }
    printf("[server] listening on port %u\n", port);

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd == -1) {
        perror("epoll_create1");
        close(listen_fd);
        return EXIT_FAILURE;
    }

    if (epoll_add(epfd, listen_fd, EPOLLIN) == -1) {
        close(epfd);
        close(listen_fd);
        return EXIT_FAILURE;
    }

    struct epoll_event events[MAX_EVENTS];

    while (!g_stop) {
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, 1000 /* ms */);
        if (nfds == -1) {
            if (errno == EINTR) {
                /* Interrupted by signal – check g_stop and loop */
                continue;
            }
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            int fd     = events[i].data.fd;
            uint32_t ev = events[i].events;

            if (fd == listen_fd) {
                /* New connection(s) available */
                accept_connections(epfd, listen_fd);
            } else {
                if (ev & (EPOLLRDHUP | EPOLLERR | EPOLLHUP)) {
                    /* Peer closed or error */
                    printf("[server] error/hangup on fd=%d, closing\n", fd);
                    epoll_del(epfd, fd);
                    close(fd);
                } else if (ev & EPOLLIN) {
                    handle_client(epfd, fd);
                }
            }
        }
    }

    printf("[server] shutting down\n");
    close(epfd);
    close(listen_fd);
    return EXIT_SUCCESS;
}
