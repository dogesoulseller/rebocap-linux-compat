// TCP client for bridge.exe.
// Frames are a uint32 LE payload length followed by RebocapMsg.
#pragma once

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "proto.hpp"

namespace rebo {
    class BridgeClient {
    public:
        using LogFn = std::function<void(const std::string &)>;

        void start(std::string host, const int port, LogFn log) {
            host_ = std::move(host);
            port_ = port;
            log_ = std::move(log);
            thread_ = std::thread([this] { run(); });
        }

        void stop() {
            stop_ = true;
            if (thread_.joinable()) {
                thread_.join();
            }
        }

        bool connected() const {
            return fd_.load() >= 0;
        }

        // Incremented on each new connection. The owner uses it to repeat its handshake.
        unsigned generation() const {
            return generation_.load();
        }

        void drain(std::vector<Msg> &out) {
            std::lock_guard lock(queue_mutex_);
            out.swap(queue_);
            queue_.clear();
        }

        // Non-blocking. If the socket buffer is full, the message is dropped. The next pose replaces it.
        bool send(const std::string &payload) {
            std::lock_guard lock(send_mutex_);

            const int fd = fd_.load();
            if (fd < 0) {
                return false;
            }

            std::string frame(4, '\0');
            const auto n = static_cast<uint32_t>(payload.size());
            std::memcpy(frame.data(), &n, 4);
            frame += payload;
            const ssize_t r = ::send(fd, frame.data(), frame.size(), MSG_NOSIGNAL | MSG_DONTWAIT);

            if (r == static_cast<ssize_t>(frame.size())) {
                return true;
            }

            if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return false;
            }

            // A partial write breaks the framing, so close the connection. run() reconnects.
            shutdown(fd, SHUT_RDWR);
            return false;
        }

    private:
        // A blocking connect to an unreachable host takes minutes, and stop() would wait for it.
        // This waits at most 3 s and returns early when stop_ is set.
        bool wait_connected(const int fd) const {
            for (int i = 0; i < 15 && !stop_; i++) {
                pollfd p{.fd = fd, .events = POLLOUT, .revents = 0};
                const int r = poll(&p, 1, 200);
                if (r < 0 && errno != EINTR) {
                    return false;
                }

                if (r > 0) {
                    int err = 0;
                    socklen_t len = sizeof err;
                    return getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0;
                }
            }

            return false;
        }

        int connect_once() const {
            addrinfo hints{}, *res = nullptr;
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;

            if (getaddrinfo(host_.c_str(), std::to_string(port_).c_str(), &hints, &res) != 0) {
                return -1;
            }

            int fd = -1;
            for (const addrinfo *a = res; a; a = a->ai_next) {
                fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, a->ai_protocol);
                if (fd < 0) {
                    continue;
                }

                if (connect(fd, a->ai_addr, a->ai_addrlen) == 0 || (errno == EINPROGRESS && wait_connected(fd))) {
                    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
                    break;
                }

                close(fd);
                fd = -1;
            }

            freeaddrinfo(res);

            if (fd >= 0) {
                constexpr int one = 1;
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            }

            return fd;
        }

        void run() {
            bool reported_failure = false;

            while (!stop_) {
                const int fd = connect_once();
                if (fd < 0) {
                    if (!reported_failure) {
                        log_("cannot reach bridge at " + host_ + ":" + std::to_string(port_) +
                             ", retrying every second");
                    }

                    reported_failure = true;

                    for (int i = 0; i < 10 && !stop_; i++) {
                        usleep(100000);
                    }

                    continue;
                }

                reported_failure = false;
                log_("connected to bridge at " + host_ + ":" + std::to_string(port_));

                ++generation_;

                fd_ = fd;
                read_loop(fd);

                {
                    std::lock_guard lock(send_mutex_);
                    fd_ = -1;
                    close(fd);
                }

                log_("bridge connection closed");
            }
        }

        void read_loop(const int fd) {
            std::vector<uint8_t> buf;
            uint8_t chunk[8192];
            while (!stop_) {
                pollfd p{.fd = fd, .events = POLLIN, .revents = 0};
                const int ready = poll(&p, 1, 200);
                if (ready == 0 || (ready < 0 && errno == EINTR)) {
                    continue;
                }

                // recv blocks and would keep stop() waiting, so it must not run after a failed poll.
                if (ready < 0) {
                    return;
                }

                const ssize_t r = recv(fd, chunk, sizeof chunk, 0);
                if (r < 0 && errno == EINTR) {
                    continue;
                }

                if (r <= 0) {
                    return;
                }

                buf.insert(buf.end(), chunk, chunk + r);
                size_t off = 0;
                while (buf.size() - off >= 4) {
                    uint32_t n;
                    std::memcpy(&n, &buf[off], 4);

                    if (n > 65536) {
                        log_("bad frame length from bridge, reconnecting");
                        return;
                    }

                    if (buf.size() - off < 4 + static_cast<size_t>(n)) {
                        break;
                    }

                    if (Msg m; decode(&buf[off + 4], n, m)) {
                        std::lock_guard lock(queue_mutex_);
                        queue_.push_back(std::move(m));
                    }

                    off += 4 + n;
                }

                buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(off));
            }
        }

        std::string host_;
        int port_ = 0;
        LogFn log_;
        std::thread thread_;
        std::atomic<bool> stop_{false};
        std::atomic<int> fd_{-1};
        std::atomic<unsigned> generation_{0};
        std::mutex queue_mutex_, send_mutex_;
        std::vector<Msg> queue_;
    };
} // namespace rebo
