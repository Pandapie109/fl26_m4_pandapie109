#pragma once

#include <QMainWindow>
#include <cstdint>
#include <memory>
#include <deque>
#include <mutex>
#include <string>

#include "aiws/async_corpus_build.hpp"
#include "aiws/ipc.hpp"
#include "aiws/processing_core.hpp"
#include "aiws/workspace.hpp"

class QLabel;
class QPushButton;
class QSocketNotifier;
class QString;
class QWidget;

namespace aiws::gui {
class CorpusState;
class ContextWidget;
class SearchWidget;
class TermInspectorWidget;
class WorkspaceWidget;

// Owns the workspace and handles the events coming from the GUI.
class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    using ExitCallback = void (*)(QWidget* parent);

    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    void set_exit_callback(ExitCallback callback) noexcept;

    // Keep these public methods for the old M3/M4/CR-1 tests.
    bool add_document_file(const QString& path);
    void set_build_delay_ms(int ms_per_document);
    bool start_ipc_server(const QString& socket_path, QString* error = nullptr);
    void set_build_factory(aiws::AsyncCorpusBuild::CoreFactory factory);

    // Used in tests to tell whether the worker has actually started a build.
    bool build_worker_active() const;
    bool build_worker_waiting() const;

private slots:
    void poll_build_completion();
    void handle_notifications();
    void remote_document_arrived(const QString& path);
    void add_document();
    void build_corpus();
    void document_selected(int row);
    void search_requested(const QString& query, int k);
    void query_changed(const QString& query);
    void build_context_requested(int token_budget);
    void source_document_requested(const QString& document_id);
    void term_inspection_requested(const QString& term, const QString& chunk_id);
    void reset_application();
    void exit_application();

private:
    friend class CorpusState;

    void update_action_states();
    void invalidate_corpus();
    void clear_processing_outputs();
    void notify_gui() noexcept;

    Workspace workspace_;
    ProcessingCore processing_core_;
    aiws::AsyncCorpusBuild builder_;
    aiws::ipc::Server ipc_server_;
    std::unique_ptr<CorpusState> state_;
    std::uint64_t workspace_revision_{0};
    int build_delay_ms_{0};
    ExitCallback exit_callback_{nullptr};

    // Worker and socket threads only write to the pipe. Qt reads it on the GUI thread.
    int notification_read_fd_{-1};
    int notification_write_fd_{-1};
    QSocketNotifier* notification_watcher_{nullptr};
    std::mutex remote_files_mutex_;
    std::deque<std::string> remote_files_;

    WorkspaceWidget* workspace_widget_{nullptr};
    SearchWidget* search_widget_{nullptr};
    TermInspectorWidget* term_inspector_widget_{nullptr};
    ContextWidget* context_widget_{nullptr};
    QLabel* status_label_{nullptr};
    QPushButton* reset_button_{nullptr};
    QPushButton* exit_button_{nullptr};
};

} // namespace aiws::gui
