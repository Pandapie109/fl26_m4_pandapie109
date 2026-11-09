#pragma once

#include "aiws/processing_core.hpp"
#include "aiws/workspace.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>

namespace aiws {

// A single-owner, non-Qt builder. All ProcessingCore::rebuild work runs on
// std::thread; the GUI polls try_take_result without blocking.
// The caller must call try_take_result after a completed build before start.
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

    explicit AsyncCorpusBuild(CoreFactory factory = {});
    ~AsyncCorpusBuild();

    AsyncCorpusBuild(const AsyncCorpusBuild&) = delete;
    AsyncCorpusBuild& operator=(const AsyncCorpusBuild&) = delete;
    AsyncCorpusBuild(AsyncCorpusBuild&&) = delete;
    AsyncCorpusBuild& operator=(AsyncCorpusBuild&&) = delete;

    // Returns false if there is an unconsumed/active build.
    // Copies of all mutable workspace content are handed to the worker.
    bool start(Workspace snapshot, std::uint64_t revision,
               int extra_ms_per_document);

    // Non-blocking. Returns true only when out receives a finished result.
    bool try_take_result(Result& out);

    bool running() const noexcept;
    void set_factory(CoreFactory factory); // GUI-thread-only, when idle
    void request_cancel() noexcept;

private:
    CoreFactory factory_;
    std::atomic<bool> cancel_{false};
    std::thread thread_;
    std::future<Result> future_;
};

}  // namespace aiws
