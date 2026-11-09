#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace aiws::ipc {

// POSIX Unix-domain stream protocol:
// request = uint32 big-endian byte count, followed by that many UTF-8 path bytes
// reply   = uint8 (0=accepted, 1=rejected), uint32 big-endian reason byte count,
//           followed by that many UTF-8 reason bytes.
// Paths may contain any UTF-8 character including newline (but not NUL).
std::string default_socket_path();
std::string absolute_file_path(const std::string& file);

class Server final {
public:
    using AddCallback = std::function<void(const std::string&)>;
    Server() = default;
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    bool start(const std::string& path, AddCallback callback, std::string& error);
    void stop() noexcept;

private:
    void run();
    void serve(int client) noexcept;

    std::atomic<bool> stopping_{false};
    int listen_fd_{-1};
    std::string path_;
    AddCallback callback_;
    std::thread thread_;
};

// Synchronous client call. The service itself does not wait for the GUI thread.
// Returns exit-status semantics: 0 accepted, 1 rejected, 2 IPC failure.
int send_add(const std::string& socket_path, const std::string& absolute_file,
             std::string& error);

} // namespace aiws::ipc
