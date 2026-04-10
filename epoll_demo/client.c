/*
 * epoll_client.c - Non-blocking TCP client demo
 *
 * Features:
 *   - Non-blocking connect with epoll-based completion detection
 *   - Sends numbered messages to the server and prints echoed replies
 *   - Graceful shutdown on SIGINT / SIGTERM
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
#include <time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>

#define DEFAULT_HOST     "127.0.0.1"
#define DEFAULT_PORT     9000
#define MAX_EVENTS       8
#define BUF_SIZE         4096
#define MSG_INTERVAL_MS  1000   /* send one message per second */
#define MAX_MESSAGES     20     /* stop after this many messages */

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

/*
 * Initiate a non-blocking connect.
 * Returns the socket fd.  The caller must wait for EPOLLOUT to confirm
 * that the connection succeeded (check SO_ERROR with getsockopt).
 * Returns -1 on error.
 */
static int nonblocking_connect(const char *host, uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        perror("socket");
        return -1;
    }

    if (set_nonblocking(fd) == -1) {
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        fprintf(stderr, "inet_pton: invalid address '%s'\n", host);
        close(fd);
        return -1;
    }

    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc == -1 && errno != EINPROGRESS) {
        perror("connect");
        close(fd);
        return -1;
    }
    /* EINPROGRESS means the connection is being established asynchronously */
    return fd;
}

/* Return monotonic time in milliseconds. */
static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(int argc, char *argv[])
{
    const char *host = DEFAULT_HOST;
    uint16_t    port = DEFAULT_PORT;

    if (argc >= 2) host = argv[1];
    if (argc >= 3) {
        long p = strtol(argv[2], NULL, 10);
        if (p <= 0 || p > 65535) {
            fprintf(stderr, "Usage: %s [host] [port]\n", argv[0]);
            return EXIT_FAILURE;
        }
        port = (uint16_t)p;
    }

    /* Install signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT,  &sa, NULL) == -1 ||
        sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("sigaction");
        return EXIT_FAILURE;
    }
    signal(SIGPIPE, SIG_IGN);

    int sock_fd = nonblocking_connect(host, port);
    if (sock_fd == -1) {
        return EXIT_FAILURE;
    }
    printf("[client] connecting to %s:%u (fd=%d)\n", host, port, sock_fd);

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd == -1) {
        perror("epoll_create1");
        close(sock_fd);
        return EXIT_FAILURE;
    }

    /* Watch for writability to detect connect completion */
    struct epoll_event ev;
    ev.events  = EPOLLOUT | EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP;
    ev.data.fd = sock_fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, sock_fd, &ev) == -1) {
        perror("epoll_ctl");
        close(epfd);
        close(sock_fd);
        return EXIT_FAILURE;
    }

    int connected      = 0;
    int msg_count      = 0;
    long long next_send = 0; /* earliest time to send the next message */
    char recv_buf[BUF_SIZE];

    struct epoll_event events[MAX_EVENTS];

    while (!g_stop && msg_count < MAX_MESSAGES) {
        long long now    = now_ms();
        int timeout_ms   = connected ? (int)(next_send - now) : 5000;
        if (timeout_ms < 0) timeout_ms = 0;

        int nfds = epoll_wait(epfd, events, MAX_EVENTS, timeout_ms);
        if (nfds == -1) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            uint32_t evmask = events[i].events;

            /* Handle error / hangup */
            if (evmask & (EPOLLERR | EPOLLHUP)) {
                int err = 0;
                socklen_t errlen = sizeof(err);
                getsockopt(sock_fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
                fprintf(stderr, "[client] socket error: %s\n", strerror(err));
                g_stop = 1;
                break;
            }

            if (!connected && (evmask & EPOLLOUT)) {
                /* Check whether the non-blocking connect succeeded */
                int err = 0;
                socklen_t errlen = sizeof(err);
                if (getsockopt(sock_fd, SOL_SOCKET, SO_ERROR, &err, &errlen) == -1 || err != 0) {
                    fprintf(stderr, "[client] connect failed: %s\n", strerror(err ? err : errno));
                    g_stop = 1;
                    break;
                }
                connected = 1;
                printf("[client] connected to %s:%u\n", host, port);
                next_send = now_ms(); /* send first message immediately */

                /* Switch to level-triggered EPOLLIN only (EPOLLOUT not needed now) */
                ev.events  = EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP;
                ev.data.fd = sock_fd;
                if (epoll_ctl(epfd, EPOLL_CTL_MOD, sock_fd, &ev) == -1) {
                    perror("epoll_ctl MOD");
                    g_stop = 1;
                    break;
                }
            }

            if (evmask & EPOLLRDHUP) {
                printf("[client] server closed the connection\n");
                g_stop = 1;
                break;
            }

            if (evmask & EPOLLIN) {
                /* Drain all available data */
                for (;;) {
                    ssize_t n = recv(sock_fd, recv_buf, sizeof(recv_buf) - 1, 0);
                    if (n == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        perror("recv");
                        g_stop = 1;
                        break;
                    }
                    if (n == 0) {
                        printf("[client] server closed the connection\n");
                        g_stop = 1;
                        break;
                    }
                    recv_buf[n] = '\0';
                    printf("[client] received: %s\n", recv_buf);
                }
            }
        }

        /* Send a new message if the interval has elapsed */
        if (!g_stop && connected) {
            long long now2 = now_ms();
            if (now2 >= next_send) {
                char msg[128];
                int len = snprintf(msg, sizeof(msg),
                                   "Hello from client, message #%d", msg_count + 1);
                ssize_t sent = 0;
                while (sent < len) {
                    ssize_t s = send(sock_fd, msg + sent, (size_t)(len - sent), MSG_NOSIGNAL);
                    if (s == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            /* Yield CPU instead of busy-waiting on a full send buffer */
                            sched_yield();
                            continue;
                        }
                        perror("send");
                        g_stop = 1;
                        break;
                    }
                    sent += s;
                }
                if (!g_stop) {
                    printf("[client] sent: %s\n", msg);
                    msg_count++;
                    next_send = now2 + MSG_INTERVAL_MS;
                }
            }
        }
    }

    if (msg_count >= MAX_MESSAGES) {
        printf("[client] sent %d messages, shutting down\n", msg_count);
    }

    close(epfd);
    close(sock_fd);
    return EXIT_SUCCESS;
}
