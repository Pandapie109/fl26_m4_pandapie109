#include "aiws/gui/corpus_state.hpp"
#include "aiws/gui/main_window.hpp"
#include "aiws/gui/workspace_widget.hpp"

#include <QString>
#include <QLabel>
#include <utility>

namespace aiws::gui {
// Each state handles the events that can change the corpus. Other events are
// ignored, such as a late result arriving after Reset.
class EmptyState final : public CorpusState {
public:
    bool can_build() const override { return false; }
    bool can_process() const override { return false; }
};

class DirtyState final : public CorpusState {
public:
    bool can_build() const override { return true; }
    bool can_process() const override { return false; }
    std::unique_ptr<CorpusState> build_requested(MainWindow& w) override {
        return begin_build(w);
    }
};

class BuildingState final : public CorpusState {
public:
    explicit BuildingState(std::uint64_t revision) : revision_(revision) {}
    bool can_build() const override { return false; }
    bool can_process() const override { return false; }
    std::unique_ptr<CorpusState> build_finished(MainWindow& w,
                                                AsyncCorpusBuild::Result& result) override;
private:
    std::uint64_t revision_;
};

class ReadyState final : public CorpusState {
public:
    bool can_build() const override { return true; }
    bool can_process() const override { return true; }
    std::unique_ptr<CorpusState> build_requested(MainWindow& w) override {
        return begin_build(w);
    }
};

std::unique_ptr<CorpusState> empty_corpus_state() {
    return std::make_unique<EmptyState>();
}

std::unique_ptr<CorpusState> CorpusState::build_requested(MainWindow&) {
    return nullptr;
}

std::unique_ptr<CorpusState> CorpusState::workspace_changed(MainWindow& w) {
    return make_dirty(w);
}

std::unique_ptr<CorpusState> CorpusState::make_dirty(MainWindow& w) {
    w.builder_.request_cancel();
    w.invalidate_corpus();
    return std::make_unique<DirtyState>();
}

std::unique_ptr<CorpusState> CorpusState::reset(MainWindow& w) {
    w.builder_.request_cancel();
    w.invalidate_corpus();
    return empty_corpus_state();
}

std::unique_ptr<CorpusState> CorpusState::begin_build(MainWindow& w) {
    w.invalidate_corpus();
    if (!w.builder_.start(w.workspace_, w.workspace_revision_, w.build_delay_ms_)) {
        w.status_label_->setText("Could not start corpus build.");
        return std::make_unique<DirtyState>();
    }
    w.workspace_widget_->set_corpus_status("Building...");
    w.status_label_->setText("Building corpus in background...");
    return std::make_unique<BuildingState>(w.workspace_revision_);
}

std::unique_ptr<CorpusState> CorpusState::build_finished(MainWindow&,
                                                          AsyncCorpusBuild::Result&) {
    return nullptr;
}

void CorpusState::publish(MainWindow& w, AsyncCorpusBuild::Result& result) {
    w.processing_core_ = std::move(*result.core);
    w.clear_processing_outputs();
    w.workspace_widget_->set_corpus_status(
        QString::number(static_cast<qulonglong>(w.processing_core_.chunk_count())) + " chunks");
    w.status_label_->setText("Corpus built.");
}

void CorpusState::failed(MainWindow& w, const std::string& error) {
    w.invalidate_corpus();
    w.status_label_->setText("Corpus build failed: " +
        QString::fromStdString(error.empty() ? "Build cancelled" : error));
}

std::unique_ptr<CorpusState> BuildingState::build_finished(
    MainWindow& w, AsyncCorpusBuild::Result& result) {
    if (result.workspace_revision != revision_) return nullptr;
    if (!result.succeeded()) {
        failed(w, result.error);
        return std::make_unique<DirtyState>();
    }
    publish(w, result);
    return std::make_unique<ReadyState>();
}

void CorpusState::exit(MainWindow& w) {
    w.builder_.stop();
}
} // namespace aiws::gui
