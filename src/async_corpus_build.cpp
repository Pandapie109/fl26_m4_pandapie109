#include "aiws/async_corpus_build.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <utility>

namespace aiws {

AsyncCorpusBuild::AsyncCorpusBuild(CoreFactory factory)
    : factory_(std::move(factory)) {
    if (!factory_) {
        factory_ = [] { return std::make_unique<ProcessingCore>(); };
    }
}

AsyncCorpusBuild::~AsyncCorpusBuild() {
    request_cancel();
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool AsyncCorpusBuild::start(Workspace snapshot, std::uint64_t revision,
                             int extra_ms_per_document) {
    if (thread_.joinable() || future_.valid()) {
        return false;
    }

    cancel_.store(false, std::memory_order_release);
    std::promise<Result> promise;
    future_ = promise.get_future();
    const auto factory = factory_; // immutable per-build copy
    const int per_document_ms = std::max(0, extra_ms_per_document);

    try {
        thread_ = std::thread(
            [this, snapshot = std::move(snapshot), revision, per_document_ms,
             factory, promise = std::move(promise)]() mutable {
                Result result;
                result.workspace_revision = revision;
                try {
                    // Delay belongs to the background build operation, once
                    // per document; do not delay any GUI event handler.
                    // Small slices make Reset/Exit cancellation prompt.
                    for (const auto& doc : snapshot.documents()) {
                        (void)doc;
                        int remaining = per_document_ms;
                        while (remaining > 0) {
                            if (cancel_.load(std::memory_order_acquire)) {
                                result.cancelled = true;
                                promise.set_value(std::move(result));
                                return;
                            }
                            const int slice_ms = std::min(remaining, 10);
                            std::this_thread::sleep_for(
                                std::chrono::milliseconds(slice_ms));
                            remaining -= slice_ms;
                        }
                    }

                    if (cancel_.load(std::memory_order_acquire)) {
                        result.cancelled = true;
                    } else {
                        auto built = factory();
                        if (!built) {
                            throw std::runtime_error("Cannot create ProcessingCore");
                        }
                        built->rebuild(snapshot);
                        if (cancel_.load(std::memory_order_acquire)) {
                            result.cancelled = true;
                        } else {
                            result.core = std::move(built);
                        }
                    }
                } catch (const std::exception& ex) {
                    result.error = ex.what();
                } catch (...) {
                    result.error = "Unknown corpus build failure";
                }
                promise.set_value(std::move(result));
            });
    } catch (...) {
        // No thread was launched. Return a ready failure to the GUI.
        std::promise<Result> fallback;
        future_ = fallback.get_future();
        Result failure;
        failure.workspace_revision = revision;
        failure.error = "Failed to start background worker";
        fallback.set_value(std::move(failure));
    }
    return true;
}

bool AsyncCorpusBuild::try_take_result(Result& out) {
    using namespace std::chrono_literals;
    if (!future_.valid() || future_.wait_for(0ms) != std::future_status::ready) {
        return false;
    }
    out = future_.get();
    if (thread_.joinable()) {
        // Promise has been fulfilled at the end of the worker function.
        thread_.join();
    }
    return true;
}

void AsyncCorpusBuild::set_factory(CoreFactory factory) {
    if (running()) throw std::logic_error("Cannot replace factory during a build");
    factory_ = factory ? std::move(factory) : CoreFactory([] {
        return std::make_unique<ProcessingCore>();
    });
}

bool AsyncCorpusBuild::running() const noexcept {
    return future_.valid(); // includes an unconsumed completed result
}

void AsyncCorpusBuild::request_cancel() noexcept {
    cancel_.store(true, std::memory_order_release);
}

}  // namespace aiws
