#include "aiws/async_corpus_build.hpp"
#include "aiws/ipc.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
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
    // The same thread is used for both successful and failed builds.
    aiws::Workspace ws;
    ws.add_document(aiws::Document("id1", "title1", "one two three four"));
    aiws::AsyncCorpusBuild worker;
    const auto id = worker.thread_id();
    if (!worker.start(ws, 21, 80)) return 1;
    aiws::AsyncCorpusBuild::Result built;
    if (!worker.wait_for_result(built, 1500ms) || !built.succeeded() ||
        built.workspace_revision != 21 || built.core->chunk_count() == 0) return 2;

    worker.set_factory([]() -> std::unique_ptr<aiws::ProcessingCore> {
        throw std::runtime_error("expected failure");
    });
    if (!worker.start(ws, 22, 0)) return 3;
    if (!worker.wait_for_result(built, 1500ms) ||
        built.error != "expected failure" || worker.thread_id() != id) return 4;
    worker.set_factory({});

    // Cancel a build after it has actually started, then hand off a new one.
    aiws::Workspace large;
    for (int i = 0; i < 6; ++i) {
        large.add_document(aiws::Document("old" + std::to_string(i), "title", "text"));
    }
    if (!worker.start(large, 23, 350)) return 25;
    // A second condition-variable notification confirms the worker is active.
    // This test does not need to guess how long thread startup takes.
    if (!worker.wait_until_active(1000ms)) return 26;
    worker.request_cancel();
    if (!worker.start(ws, 24, 0)) return 27;
    const auto start = std::chrono::steady_clock::now();
    bool got_new_build = false;
    while (std::chrono::steady_clock::now() - start < 1400ms) {
        if (!worker.wait_for_result(built, 1400ms)) break;
        if (built.workspace_revision == 24 && built.succeeded()) {
            got_new_build = true;
            break;
        }
    }
    if (!got_new_build || worker.thread_id() != id) return 28;
    if (!worker.wait_until_waiting(1000ms)) return 29;

    // Exit should wake a worker even during a long document delay.
    auto exiting = std::make_unique<aiws::AsyncCorpusBuild>();
    if (!exiting->start(large, 30, 1500)) return 30;
    if (!exiting->wait_until_active(1000ms)) return 31;
    const auto exit_start = std::chrono::steady_clock::now();
    exiting.reset();
    if (std::chrono::steady_clock::now() - exit_start > 900ms) return 32;

    char dirname[] = "/tmp/m4-ipc-test-XXXXXX";
    if (!::mkdtemp(dirname)) return 6;
    const std::string filepath = std::string(dirname) + "/name with newline\nand utf8_日本.txt";
    const std::string socket = std::string(dirname) + "/socket.sock";
    { std::ofstream out(filepath); out << "document"; }
    std::promise<std::string> observed;
    std::mutex adds_mutex;
    std::condition_variable adds_cv;
    int adds = 0;
    auto future = observed.get_future();
    aiws::ipc::Server server;
    std::string error;
    if (!server.start(socket, [&observed, &adds, &adds_mutex, &adds_cv](const std::string& path) {
          std::lock_guard<std::mutex> lock(adds_mutex);
          if (++adds == 1) observed.set_value(path);
          adds_cv.notify_all();
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
    {
        std::unique_lock<std::mutex> lock(adds_mutex);
        if (!adds_cv.wait_for(lock, 1000ms, [&adds] { return adds == 2; })) return 21;
    }
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
