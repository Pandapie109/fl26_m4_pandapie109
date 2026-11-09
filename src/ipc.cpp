#include "aiws/ipc.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <fcntl.h>
#include <unistd.h>

namespace aiws::ipc {
namespace {
constexpr std::size_t kMaxPathBytes = 1024 * 1024;
using Clock = std::chrono::steady_clock;

std::string system_error(const char* description) {
    return std::string(description) + ": " + std::strerror(errno);
}

bool socket_address(const std::string& path, sockaddr_un& addr, socklen_t& length) {
    if (path.empty() || path.find('\0') != std::string::npos ||
        path.size() >= sizeof(addr.sun_path)) return false;
    addr = {};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
    return true;
}

// Poll in short slices so Server::stop interrupts a partial peer request.
bool ready(int fd, short events, Clock::time_point deadline,
           const std::atomic<bool>* stop = nullptr) {
    while (Clock::now() < deadline && !(stop && stop->load())) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (remaining <= 0) return false;
        pollfd pfd{fd, events, 0};
        int result = ::poll(&pfd, 1, static_cast<int>(remaining < 50 ? remaining : 50));
        if (result > 0) return (pfd.revents & (events | POLLHUP | POLLERR | POLLNVAL)) != 0;
        if (result < 0 && errno != EINTR) return false;
    }
    return false;
}

bool read_all(int fd, void* dest, std::size_t n, Clock::time_point deadline,
              const std::atomic<bool>* stop = nullptr) {
    char* p = static_cast<char*>(dest);
    while (n) {
        if (!ready(fd, POLLIN, deadline, stop)) return false;
        const ssize_t got = ::recv(fd, p, n, MSG_DONTWAIT);
        if (got == 0) return false;
        if (got < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return false;
        }
        p += got;
        n -= static_cast<std::size_t>(got);
    }
    return true;
}

bool write_all(int fd, const void* src, std::size_t n, Clock::time_point deadline,
               const std::atomic<bool>* stop = nullptr) {
    const char* p = static_cast<const char*>(src);
    while (n) {
        if (!ready(fd, POLLOUT, deadline, stop)) return false;
        const ssize_t sent = ::send(fd, p, n, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return false;
        }
        if (sent == 0) return false;
        p += sent;
        n -= static_cast<std::size_t>(sent);
    }
    return true;
}

bool send_reply(int fd, unsigned char status, const std::string& reason,
                Clock::time_point deadline) {
    const std::uint32_t length = htonl(static_cast<std::uint32_t>(reason.size()));
    return write_all(fd, &status, 1, deadline) &&
           write_all(fd, &length, sizeof(length), deadline) &&
           write_all(fd, reason.data(), reason.size(), deadline);
}

bool readable_file(const std::string& path, std::string& reason) {
    if (path.empty() || path.find('\0') != std::string::npos) {
        reason = "Invalid path";
        return false;
    }
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        reason = system_error("Cannot open file");
        return false;
    }
    struct stat st{};
    const bool good = (::fstat(fd, &st) == 0 && S_ISREG(st.st_mode));
    ::close(fd);
    if (!good) reason = "Not a readable regular file";
    return good;
}
} // namespace

std::string default_socket_path() {
    const char* user = std::getenv("USER");
    return "/tmp/aiws-" + (user && *user ? std::string(user) : std::to_string(::getuid())) + ".sock";
}

std::string absolute_file_path(const std::string& file) {
    return std::filesystem::absolute(std::filesystem::path(file)).lexically_normal().string();
}

Server::~Server() { stop(); }

bool Server::start(const std::string& path, AddCallback callback, std::string& error) {
    if (listen_fd_ != -1) { error = "IPC server is already running"; return false; }
    sockaddr_un address{};
    socklen_t len = 0;
    if (!socket_address(path, address, len)) { error = "Invalid or overlong socket path"; return false; }

    // Only remove an abandoned socket; do not steal another live application's socket.
    struct stat st{};
    if (::lstat(path.c_str(), &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) { error = "Socket path is occupied by another file"; return false; }
        int probe = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        bool live = false;
        if (probe >= 0) {
            live = ::connect(probe, reinterpret_cast<const sockaddr*>(&address), len) == 0;
            ::close(probe);
        }
        if (live) { error = "Another AI Workspace is serving this socket"; return false; }
        if (::unlink(path.c_str()) != 0) { error = system_error("Cannot remove stale socket"); return false; }
    }

    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { error = system_error("Cannot create socket"); return false; }
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), len) < 0) {
        error = system_error("Cannot bind socket");
        ::close(fd);
        return false;
    }
    if (::listen(fd, 16) < 0) {
        error = system_error("Cannot listen on socket");
        ::close(fd);
        ::unlink(path.c_str());
        return false;
    }
    listen_fd_ = fd;
    path_ = path;
    callback_ = std::move(callback);
    stopping_.store(false);
    try {
        thread_ = std::thread(&Server::run, this);
    } catch (const std::exception& e) {
        error = std::string("Cannot start IPC thread: ") + e.what();
        ::close(fd);
        ::unlink(path.c_str());
        listen_fd_ = -1;
        path_.clear();
        return false;
    }
    return true;
}

void Server::stop() noexcept {
    stopping_.store(true);
    if (thread_.joinable()) thread_.join();
    if (listen_fd_ >= 0) { ::close(listen_fd_); listen_fd_ = -1; }
    if (!path_.empty()) { ::unlink(path_.c_str()); path_.clear(); }
}

void Server::run() {
    while (!stopping_.load()) {
        pollfd pfd{listen_fd_, POLLIN, 0};
        const int status = ::poll(&pfd, 1, 50);
        if (status < 0) { if (errno == EINTR) continue; break; }
        if (status == 0) continue;
        if (!(pfd.revents & POLLIN)) continue;
        const int client = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (client < 0) continue;
        serve(client);
        ::close(client);
    }
}

void Server::serve(int client) noexcept {
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    std::uint32_t net_size = 0;
    if (!read_all(client, &net_size, sizeof(net_size), deadline, &stopping_)) return;
    const std::uint32_t length = ntohl(net_size);
    if (length == 0 || length > kMaxPathBytes) {
        send_reply(client, 1, "Invalid path length", deadline);
        return;
    }
    std::string path(length, '\0');
    if (!read_all(client, path.data(), length, deadline, &stopping_)) return;
    std::string reason;
    if (!readable_file(path, reason)) {
        send_reply(client, 1, reason, deadline);
        return;
    }
    try {
        // Queue GUI modification only after validating the path. No GUI wait here.
        callback_(path);
        send_reply(client, 0, "", deadline);
    } catch (...) {
        send_reply(client, 1, "Could not schedule document addition", deadline);
    }
}

int send_add(const std::string& socket_path, const std::string& absolute_file, std::string& error) {
    sockaddr_un address{};
    socklen_t len = 0;
    if (!socket_address(socket_path, address, len)) { error = "Invalid socket path"; return 2; }
    if (absolute_file.empty() || absolute_file.size() > kMaxPathBytes ||
        absolute_file.find('\0') != std::string::npos) {
        error = "Invalid file path";
        return 1;
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) { error = system_error("Cannot open socket"); return 2; }
    int result = 2;
    const auto connect_deadline = Clock::now() + std::chrono::seconds(2);
    const int connected = ::connect(fd, reinterpret_cast<const sockaddr*>(&address), len);
    if (connected != 0) {
        if (errno != EINPROGRESS || !ready(fd, POLLOUT, connect_deadline)) {
            error = system_error("Cannot reach AI Workspace");
            goto done;
        }
        int so_error = 0;
        socklen_t size = sizeof(so_error);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &size) != 0 || so_error != 0) {
            if (so_error) errno = so_error;
            error = system_error("Cannot connect to AI Workspace");
            goto done;
        }
    }
    {
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        const std::uint32_t length = htonl(static_cast<std::uint32_t>(absolute_file.size()));
        unsigned char status = 0xff;
        std::uint32_t response_length = 0;
        if (!write_all(fd, &length, sizeof(length), deadline) ||
            !write_all(fd, absolute_file.data(), absolute_file.size(), deadline) ||
            !read_all(fd, &status, 1, deadline) ||
            !read_all(fd, &response_length, sizeof(response_length), deadline)) {
            error = "Communication with AI Workspace failed";
            goto done;
        }
        response_length = ntohl(response_length);
        if (response_length > kMaxPathBytes) { error = "Malformed server response"; goto done; }
        if (response_length) {
            std::string message(response_length, '\0');
            if (!read_all(fd, message.data(), message.size(), deadline)) {
                error = "Incomplete server response";
                goto done;
            }
            error = std::move(message);
        }
        result = status == 0 ? 0 : status == 1 ? 1 : 2;
        if (result == 2) error = "Invalid server response";
    }
done:
    ::close(fd);
    return result;
}

} // namespace aiws::ipc
