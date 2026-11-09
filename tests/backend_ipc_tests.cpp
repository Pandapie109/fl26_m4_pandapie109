#include "aiws/async_corpus_build.hpp"
#include "aiws/ipc.hpp"

#include <atomic>
#include <chrono>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace std::chrono_literals;

int main() {
    // Verify that real ProcessingCore work happens on a separate std::thread.
    aiws::Workspace ws;
    ws.add_document(aiws::Document("id1", "title1", "one two three four"));
    aiws::AsyncCorpusBuild worker;
    if (!worker.start(ws, 21, 80)) return 1;
    if (!worker.running()) return 2;
    aiws::AsyncCorpusBuild::Result built;
    bool completed = false;
    for (int i = 0; i < 200; ++i) {
        if (worker.try_take_result(built)) { completed = true; break; }
        std::this_thread::sleep_for(10ms);
    }
    if (!completed || !built.succeeded() || built.workspace_revision != 21 ||
        built.core->chunk_count() == 0) return 3;

    worker.set_factory([]() -> std::unique_ptr<aiws::ProcessingCore> {
        throw std::runtime_error("expected failure");
    });
    if (!worker.start(ws, 22, 0)) return 4;
    completed = false;
    for (int i = 0; i < 200; ++i) {
        if (worker.try_take_result(built)) { completed = true; break; }
        std::this_thread::sleep_for(10ms);
    }
    if (!completed || built.error != "expected failure") return 5;

    char dirname[] = "/tmp/m4-ipc-test-XXXXXX";
    if (!::mkdtemp(dirname)) return 6;
    const std::string filepath = std::string(dirname) + "/name with newline\nand utf8_日本.txt";
    const std::string socket = std::string(dirname) + "/socket.sock";
    { std::ofstream out(filepath); out << "document"; }
    std::promise<std::string> observed;
    std::atomic<int> adds{0};
    auto future = observed.get_future();
    aiws::ipc::Server server;
    std::string error;
    if (!server.start(socket, [&observed, &adds](const std::string& path) {
          if (++adds == 1) observed.set_value(path);
        }, error)) {
        std::cerr << error << '\n'; return 7;
    }
    if (aiws::ipc::send_add(socket, filepath, error) != 0) return 8;
    if (future.wait_for(1s) != std::future_status::ready || future.get() != filepath) return 9;
    if (aiws::ipc::send_add(socket, filepath + "not-found", error) != 1) return 10;

    // Split the prefix and payload across four writes, including pauses.
    const int client = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (client < 0) return 13;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket.size() >= sizeof(addr.sun_path)) return 14;
    std::memcpy(addr.sun_path, socket.c_str(), socket.size() + 1);
    if (::connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) return 15;
    const auto net_length = htonl(static_cast<std::uint32_t>(filepath.size()));
    const char* len = reinterpret_cast<const char*>(&net_length);
    if (::send(client, len, 1, 0) != 1) return 16;
    std::this_thread::sleep_for(20ms);
    if (::send(client, len + 1, 3, 0) != 3) return 17;
    std::this_thread::sleep_for(20ms);
    if (::send(client, filepath.data(), 4, 0) != 4) return 18;
    std::this_thread::sleep_for(20ms);
    if (::send(client, filepath.data() + 4, filepath.size() - 4, 0)
          != static_cast<ssize_t>(filepath.size() - 4)) return 19;
    unsigned char accepted = 255;
    if (::recv(client, &accepted, 1, 0) != 1 || accepted != 0) return 20;
    ::close(client);
    for (int i = 0; i < 100 && adds.load() < 2; ++i) std::this_thread::sleep_for(10ms);
    if (adds.load() != 2) return 21;
    // Shutdown must interrupt even a client holding a partial request open.
    const int stalled = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (stalled < 0) return 22;
    if (::connect(stalled, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) return 23;
    std::this_thread::sleep_for(70ms);
    const auto stop_start = std::chrono::steady_clock::now();
    server.stop();
    if (std::chrono::steady_clock::now() - stop_start > 500ms) return 24;
    ::close(stalled);
    struct stat st{};
    if (::lstat(socket.c_str(), &st) == 0) return 11;
    if (aiws::ipc::send_add(socket, filepath, error) != 2) return 12;
    ::unlink(filepath.c_str());
    ::rmdir(dirname);
    std::cout << "Backend build and IPC smoke tests passed\n";
    return 0;
}
