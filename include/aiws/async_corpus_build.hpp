#pragma once

#include "aiws/processing_core.hpp"
#include "aiws/workspace.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace aiws {

// Keeps one worker alive instead of starting another thread for each build.
class AsyncCorpusBuild final {
public:
    struct Result {
        std::uint64_t workspace_revision{0};
        std::unique_ptr<ProcessingCore> core;
        std::string error;
        bool cancelled{false};

        bool succeeded() const noexcept {
            return core != nullptr && error.empty() && !cancelled;
        }
    };

    using CoreFactory = std::function<std::unique_ptr<ProcessingCore>()>;
    using FinishedNotice = std::function<void()>;

    explicit AsyncCorpusBuild(CoreFactory factory = {}, FinishedNotice notice = {});
    ~AsyncCorpusBuild();

    AsyncCorpusBuild(const AsyncCorpusBuild&) = delete;
    AsyncCorpusBuild& operator=(const AsyncCorpusBuild&) = delete;

    // A newer request replaces one that the worker has not picked up yet.
    bool start(Workspace snapshot, std::uint64_t revision, int extra_ms_per_document);
    void request_cancel() noexcept;
    bool try_take_result(Result& out);
    bool wait_for_result(Result& out, std::chrono::milliseconds timeout);
    bool running() const;
    bool active() const;
    bool waiting() const;
    bool wait_until_waiting(std::chrono::milliseconds timeout);
    bool wait_until_active(std::chrono::milliseconds timeout);
    std::thread::id thread_id() const noexcept;
    void set_factory(CoreFactory factory);
    void stop() noexcept;

private:
    struct Request {
        Workspace snapshot;
        std::uint64_t revision{0};
        std::uint64_t generation{0};
        int delay_ms{0};
        CoreFactory factory;
    };

    void work_loop();
    bool cancelled(std::uint64_t generation) const noexcept;
    void wait_for_delay(std::uint64_t generation, int delay_ms);

    // These two are atomic because the GUI can change them during a build.
    std::atomic<std::uint64_t> generation_{0};
    std::atomic<bool> stopping_{false};

    // The request, results, and worker status are all protected by mutex_.
    mutable std::mutex mutex_;
    std::condition_variable request_cv_;
    std::condition_variable delay_cv_;
    std::condition_variable result_cv_;
    std::optional<Request> pending_;
    std::deque<Result> results_;
    bool active_{false};
    bool waiting_{false};
    CoreFactory factory_;
    FinishedNotice notice_;
    std::thread worker_;
};

} // namespace aiws
