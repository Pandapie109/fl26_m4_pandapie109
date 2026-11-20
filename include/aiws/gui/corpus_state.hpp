#pragma once

#include "aiws/async_corpus_build.hpp"
#include <memory>

namespace aiws::gui {
class MainWindow;

// GUI-thread-only states for the corpus lifecycle.
class CorpusState {
public:
    virtual ~CorpusState() = default;
    virtual bool can_build() const = 0;
    virtual bool can_process() const = 0;

    virtual std::unique_ptr<CorpusState> build_requested(MainWindow& window);
    virtual std::unique_ptr<CorpusState> workspace_changed(MainWindow& window);
    virtual std::unique_ptr<CorpusState> reset(MainWindow& window);
    virtual std::unique_ptr<CorpusState> build_finished(MainWindow& window,
                                                        AsyncCorpusBuild::Result& result);
    virtual void exit(MainWindow& window);

protected:
    std::unique_ptr<CorpusState> begin_build(MainWindow& window);
    std::unique_ptr<CorpusState> make_dirty(MainWindow& window);
    void publish(MainWindow& window, AsyncCorpusBuild::Result& result);
    void failed(MainWindow& window, const std::string& error);
};

std::unique_ptr<CorpusState> empty_corpus_state();
}
