#include "aiws/async_corpus_build.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

namespace aiws {

AsyncCorpusBuild::AsyncCorpusBuild(CoreFactory factory, FinishedNotice notice)
    : factory_(factory ? std::move(factory) : CoreFactory([] {
          return std::make_unique<ProcessingCore>();
      })),
      notice_(std::move(notice)),
      worker_(&AsyncCorpusBuild::work_loop, this) {}

AsyncCorpusBuild::~AsyncCorpusBuild() {
    stop();
}

bool AsyncCorpusBuild::start(Workspace snapshot, std::uint64_t revision,
                             int extra_ms_per_document) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_.load()) return false;

    // Incrementing the generation also tells an older build to stop.
    const auto next = generation_.fetch_add(1) + 1;
    pending_ = Request{std::move(snapshot), revision, next,
                       std::max(0, extra_ms_per_document), factory_};
    request_cv_.notify_one();
    delay_cv_.notify_all();
    return true;
}

void AsyncCorpusBuild::request_cancel() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++generation_;
        pending_.reset();
    }
    delay_cv_.notify_all();
}

bool AsyncCorpusBuild::cancelled(std::uint64_t generation) const noexcept {
    if (stopping_.load(std::memory_order_acquire)) return true;
#ifdef AIWS_FAULT_NO_CANCEL
    // Fault for the M5 tests: a changed workspace no longer interrupts a build.
    (void)generation;
    return false;
#else
    return generation_.load(std::memory_order_acquire) != generation;
#endif
}

void AsyncCorpusBuild::wait_for_delay(std::uint64_t generation, int delay_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    delay_cv_.wait_for(lock, std::chrono::milliseconds(delay_ms), [this, generation] {
        return cancelled(generation);
    });
}

void AsyncCorpusBuild::work_loop() {
    for (;;) {
        Request request;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            waiting_ = true;
            result_cv_.notify_all();
            request_cv_.wait(lock, [this] {
                return stopping_.load() || pending_.has_value();
            });
            waiting_ = false;
            if (stopping_.load()) return;
            request = std::move(*pending_);
            pending_.reset();
            active_ = true;
            result_cv_.notify_all();
        }

        Result result;
        result.workspace_revision = request.revision;
        try {
            if (cancelled(request.generation)) {
                result.cancelled = true;
            } else {
                auto core = request.factory();
                if (!core) throw std::runtime_error("Cannot create ProcessingCore");
                // The core checks for cancellation after each document.
                if (core->rebuild_cancelable(request.snapshot,
                        [this, generation = request.generation] {
                            return cancelled(generation);
                        }, request.delay_ms,
                        [this, generation = request.generation](int delay_ms) {
                            wait_for_delay(generation, delay_ms);
                        }) && !cancelled(request.generation)) {
                    result.core = std::move(core);
                } else {
                    result.cancelled = true;
                }
            }
        } catch (const std::exception& e) {
            result.error = e.what();
        } catch (...) {
            result.error = "Unknown corpus build error";
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            active_ = false;
            results_.push_back(std::move(result));
        }
        result_cv_.notify_all();
        if (notice_) notice_();  // MainWindow queues a slot on the GUI thread.
    }
}

bool AsyncCorpusBuild::try_take_result(Result& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (results_.empty()) return false;
    out = std::move(results_.front());
    results_.pop_front();
    return true;
}

bool AsyncCorpusBuild::wait_for_result(Result& out, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!result_cv_.wait_for(lock, timeout, [this] { return !results_.empty(); })) {
        return false;
    }
    out = std::move(results_.front());
    results_.pop_front();
    return true;
}

bool AsyncCorpusBuild::running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_ || pending_.has_value() || !results_.empty();
}

bool AsyncCorpusBuild::active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_;
}

bool AsyncCorpusBuild::waiting() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return waiting_;
}

bool AsyncCorpusBuild::wait_until_waiting(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return result_cv_.wait_for(lock, timeout, [this] { return waiting_; });
}

bool AsyncCorpusBuild::wait_until_active(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return result_cv_.wait_for(lock, timeout, [this] { return active_; });
}

std::thread::id AsyncCorpusBuild::thread_id() const noexcept {
    return worker_.get_id();
}

void AsyncCorpusBuild::set_factory(CoreFactory factory) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_ || pending_.has_value()) {
        throw std::logic_error("Cannot change the factory during a build");
    }
    factory_ = factory ? std::move(factory) : CoreFactory([] {
        return std::make_unique<ProcessingCore>();
    });
}

void AsyncCorpusBuild::stop() noexcept {
    if (!worker_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_.store(true);
        ++generation_;
        pending_.reset();
    }
#ifndef AIWS_FAULT_SILENT_SHUTDOWN
    request_cv_.notify_all();
#endif
    // This also wakes a worker that is in the middle of a document delay.
    delay_cv_.notify_all();
    // The missing request notification above makes idle shutdown hang.
    worker_.join();
}

} // namespace aiws
