/*
 * Rebocap pipe <-> TCP bridge. Runs under Wine next to rebocap.exe.
 *
 * The Rebocap app serves \\.\pipe\Rebocap2048VRD and expects its SteamVR driver to connect as a message-mode client.
 * This program listens on TCP and for each TCP client opens the pipe and relays messages unchanged in both directions.
 * A native driver, local or on another machine, can then replace the Windows driver.
 *
 * Format on both sides: uint32 LE payload length, then serialized rebocap_pb.RebocapMsg.
 * A pipe write must contain the header and payload in one WriteFile.
 *
 * usage: bridge.exe [bind_addr] [port]     defaults: 127.0.0.1 36850
 */

/* winsock2.h must come before windows.h. The blank lines keep clang-format from reordering them. */
#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define PIPE_NAME "\\\\.\\pipe\\Rebocap2048VRD"
#define MAX_PAYLOAD 2044
#define MAX_MSG (MAX_PAYLOAD + 4)

struct session {
    SOCKET sock;
    HANDLE pipe;
    volatile LONG closing;
};

static void logmsg(const char *fmt, ...) {
    va_list ap;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(stderr, "[bridge %02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* Overlapped I/O is required. A synchronous pipe handle serializes reads and writes,
 * so a blocked ReadFile would also block WriteFile on the other thread. */
static BOOL pipe_io(HANDLE pipe, BOOL write, void *buf, DWORD len, DWORD *done) {
    OVERLAPPED ov = {0};
    BOOL ok;
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    ok = write ? WriteFile(pipe, buf, len, NULL, &ov) : ReadFile(pipe, buf, len, NULL, &ov);
    if (!ok && GetLastError() != ERROR_IO_PENDING) {
        CloseHandle(ov.hEvent);
        return FALSE;
    }

    ok = GetOverlappedResult(pipe, &ov, done, TRUE);
    CloseHandle(ov.hEvent);

    return ok;
}

static void end_session(struct session *s) {
    if (InterlockedExchange(&s->closing, 1)) {
        return;
    }

    shutdown(s->sock, SD_BOTH);
    CancelIoEx(s->pipe, NULL);
}

static DWORD WINAPI pipe_to_sock(void *arg) {
    struct session *s = arg;
    uint8_t buf[4096];
    DWORD n;

    while (!s->closing) {
        if (!pipe_io(s->pipe, FALSE, buf, sizeof buf, &n)) {
            if (!s->closing) {
                logmsg("pipe read failed, error %lu", GetLastError());
            }

            break;
        }

        if (n < 4 || *(uint32_t *)buf != n - 4) {
            logmsg("dropping malformed pipe message: %lu bytes, header says %u", n, n >= 4 ? *(uint32_t *)buf : 0);
            continue;
        }

        if (send(s->sock, (char *)buf, (int)n, 0) != (int)n) {
            if (!s->closing) {
                logmsg("tcp send failed, error %d", WSAGetLastError());
            }

            break;
        }
    }

    end_session(s);

    return 0;
}

static BOOL recv_all(SOCKET sock, uint8_t *buf, int len) {
    while (len > 0) {
        int r = recv(sock, (char *)buf, len, 0);
        if (r <= 0) {
            return FALSE;
        }

        buf += r;
        len -= r;
    }

    return TRUE;
}

static void sock_to_pipe(struct session *s) {
    uint8_t buf[MAX_MSG];
    uint32_t len;
    DWORD n;

    while (!s->closing) {
        if (!recv_all(s->sock, buf, 4)) {
            break;
        }

        len = *(uint32_t *)buf;
        if (len > MAX_PAYLOAD) {
            logmsg("tcp client sent oversized message (%u bytes), closing", len);
            break;
        }

        if (!recv_all(s->sock, buf + 4, (int)len)) {
            break;
        }

        if (!pipe_io(s->pipe, TRUE, buf, len + 4, &n) || n != len + 4) {
            if (!s->closing) {
                logmsg("pipe write failed, error %lu", GetLastError());
            }

            break;
        }
    }

    end_session(s);
}

/* The app may not be running yet. Retry until the TCP client disconnects. */
static HANDLE open_pipe(SOCKET sock) {
    for (;;) {
        DWORD mode = PIPE_READMODE_MESSAGE;
        HANDLE pipe =
            CreateFileA(PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);

        if (pipe != INVALID_HANDLE_VALUE) {
            if (!SetNamedPipeHandleState(pipe, &mode, NULL, NULL)) {
                logmsg("warning: cannot set message read mode, error %lu", GetLastError());
            }

            return pipe;
        }

        if (GetLastError() == ERROR_PIPE_BUSY) {
            WaitNamedPipeA(PIPE_NAME, 1000);
        } else {
            char c;
            fd_set rd;
            struct timeval tv = {1, 0};
            FD_ZERO(&rd);
            FD_SET(sock, &rd);

            if (select(0, &rd, NULL, NULL, &tv) > 0 && recv(sock, &c, 1, MSG_PEEK) <= 0) {
                return INVALID_HANDLE_VALUE;
            }
        }
    }
}

int main(int argc, char **argv) {
    const char *bind_addr = argc > 1 ? argv[1] : "127.0.0.1";
    int port = argc > 2 ? atoi(argv[2]) : 36850;

    struct sockaddr_in addr = {0};
    WSADATA wsa;
    SOCKET srv;
    BOOL one = TRUE;

    WSAStartup(MAKEWORD(2, 2), &wsa);
    srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char *)&one, sizeof one);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);

    if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
        logmsg("bad bind address %s", bind_addr);
        return 1;
    }

    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) || listen(srv, 1)) {
        logmsg("cannot listen on %s:%d, error %d", bind_addr, port, WSAGetLastError());
        return 1;
    }

    logmsg("listening on %s:%d", bind_addr, port);

    for (;;) {
        struct session s = {0};
        HANDLE thread;

        s.sock = accept(srv, NULL, NULL);
        if (s.sock == INVALID_SOCKET) {
            continue;
        }

        setsockopt(s.sock, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof one);
        logmsg("driver connected, opening %s", PIPE_NAME);

        /* Open the pipe only while a driver is connected. The app then sees a driver
         * connection only when a driver exists. */
        s.pipe = open_pipe(s.sock);
        if (s.pipe != INVALID_HANDLE_VALUE) {
            logmsg("pipe open, relaying");
            thread = CreateThread(NULL, 0, pipe_to_sock, &s, 0, NULL);
            sock_to_pipe(&s);
            WaitForSingleObject(thread, INFINITE);
            CloseHandle(thread);
            CloseHandle(s.pipe);
        }

        closesocket(s.sock);
        logmsg("session ended");
    }
}
