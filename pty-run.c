#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static struct termios saved_termios;
static bool termios_saved = false;
static volatile sig_atomic_t caught_signal = 0;
static int master_fd = -1;
static pid_t child_pid = -1;

static void restore_terminal(void) {
    if (termios_saved && isatty(STDIN_FILENO))
        (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios);
}

static void signal_handler(int signo) {
    caught_signal = signo;
}

static int set_raw_mode(void) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "pty-run: stdin and stdout must be terminals\n");
        return -1;
    }
    if (tcgetattr(STDIN_FILENO, &saved_termios) == -1) {
        perror("tcgetattr");
        return -1;
    }
    termios_saved = true;
    struct termios raw = saved_termios;
    cfmakeraw(&raw);
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        perror("tcsetattr(raw)");
        return -1;
    }
    return 0;
}

static int write_all(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n > 0) { off += (size_t)n; continue; }
        if (n == -1 && errno == EINTR) continue;
        if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd p = {.fd = fd, .events = POLLOUT};
            if (poll(&p, 1, -1) == -1 && errno != EINTR) return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

static int relay_loop(int fd) {
    struct pollfd pfds[2] = {
        {.fd = STDIN_FILENO, .events = POLLIN},
        {.fd = fd, .events = POLLIN}
    };
    char buf[4096];

    while (!caught_signal) {
        int rc = poll(pfds, 2, 100);
        if (rc == -1) {
            if (errno == EINTR) continue;
            perror("poll");
            return -1;
        }
        if (rc == 0) continue;

        if (pfds[0].revents & (POLLIN | POLLHUP)) {
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n == 0) {
                /* Host stdin closed: send EOF to the PTY's foreground input. */
                const char eof = 4; /* Ctrl-D byte; in raw host mode it is data. */
                if (write_all(fd, &eof, 1) == -1) return -1;
                pfds[0].fd = -1;
            } else if (n > 0) {
                if (write_all(fd, buf, (size_t)n) == -1) {
                    if (errno == EIO) return 0;
                    perror("write PTY");
                    return -1;
                }
            } else if (errno != EINTR && errno != EAGAIN) {
                perror("read stdin");
                return -1;
            }
        }
        if (pfds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n > 0) {
                if (write_all(STDOUT_FILENO, buf, (size_t)n) == -1) {
                    perror("write stdout");
                    return -1;
                }
            } else if (n == 0 || (n == -1 && errno == EIO)) {
                return 0; /* Linux PTY master commonly returns EIO at slave close. */
            } else if (errno != EINTR && errno != EAGAIN) {
                perror("read PTY");
                return -1;
            }
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s PROGRAM [ARG...]\n", argv[0]);
        return 2;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGWINCH, &sa, NULL);

    int slave_fd = -1;
    if (openpty(&master_fd, &slave_fd, NULL, NULL, NULL) == -1) {
        perror("openpty");
        return 1;
    }

    child_pid = fork();
    if (child_pid == -1) {
        perror("fork");
        close(master_fd); close(slave_fd);
        return 1;
    }

    if (child_pid == 0) {
        close(master_fd);
        if (setsid() == -1) { perror("setsid"); _exit(126); }
        if (ioctl(slave_fd, TIOCSCTTY, 0) == -1) {
            perror("ioctl(TIOCSCTTY)"); _exit(126);
        }
        if (dup2(slave_fd, STDIN_FILENO) == -1 ||
            dup2(slave_fd, STDOUT_FILENO) == -1 ||
            dup2(slave_fd, STDERR_FILENO) == -1) {
            perror("dup2"); _exit(126);
        }
        if (slave_fd > STDERR_FILENO) close(slave_fd);
        execvp(argv[1], &argv[1]);
        perror("execvp");
        _exit(errno == ENOENT ? 127 : 126);
    }

    close(slave_fd);
    if (set_raw_mode() == -1) {
        kill(child_pid, SIGTERM);
        waitpid(child_pid, NULL, 0);
        close(master_fd);
        return 1;
    }

    /* Forward window-size changes to the PTY. */
    struct sigaction winch_sa;
    memset(&winch_sa, 0, sizeof(winch_sa));
    winch_sa.sa_handler = signal_handler;
    sigemptyset(&winch_sa.sa_mask);
    sigaction(SIGWINCH, &winch_sa, NULL);

    int relay_rc = relay_loop(master_fd);
    restore_terminal();

    if (caught_signal) kill(child_pid, caught_signal);
    if (relay_rc == -1) kill(child_pid, SIGTERM);

    int status = 0;
    while (waitpid(child_pid, &status, 0) == -1 && errno == EINTR) {}
    close(master_fd);

    if (caught_signal) {
        signal(caught_signal, SIG_DFL);
        raise(caught_signal);
    }
    if (relay_rc == -1) return 1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 0;
}
