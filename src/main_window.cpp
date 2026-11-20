#include "aiws/gui/main_window.hpp"

#include "aiws/gui/context_widget.hpp"
#include "aiws/gui/corpus_state.hpp"
#include "aiws/gui/resettable.hpp"
#include "aiws/gui/search_widget.hpp"
#include "aiws/gui/term_inspector_widget.hpp"
#include "aiws/gui/workspace_widget.hpp"

#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSocketNotifier>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <exception>
#include <stdexcept>
#include <utility>
#include <vector>

namespace aiws::gui {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      builder_({}, [this] { notify_gui(); }),
      state_(empty_corpus_state()) {
    // The pipe lets background threads wake the GUI without calling Qt.
    int fds[2];
    if (::pipe2(fds, O_NONBLOCK | O_CLOEXEC) != 0) {
        throw std::runtime_error(std::string("Cannot create notification pipe: ") +
                                 std::strerror(errno));
    }
    notification_read_fd_ = fds[0];
    notification_write_fd_ = fds[1];
    notification_watcher_ = new QSocketNotifier(notification_read_fd_,
                                                QSocketNotifier::Read, this);
    connect(notification_watcher_,
            &QSocketNotifier::activated,
            this, &MainWindow::handle_notifications);

    setWindowTitle("AI Workspace");
    resize(900, 900);

    auto* central = new QWidget(this);
    setCentralWidget(central);

    workspace_widget_ = new WorkspaceWidget(central);
    search_widget_ = new SearchWidget(central);
    term_inspector_widget_ = new TermInspectorWidget(central);
    context_widget_ = new ContextWidget(central);
    status_label_ = new QLabel("Ready", central);
    status_label_->setObjectName("statusLabel");
    reset_button_ = new QPushButton("Reset", central);
    reset_button_->setObjectName("resetButton");
    exit_button_ = new QPushButton("Exit", central);
    exit_button_->setObjectName("exitButton");

    auto* header = new QLabel("AI Workspace\nDocument Search and Context Explorer", central);
    header->setStyleSheet(
        "background-color: #681d32; color: white; padding: 12px; "
        "font-size: 17px; font-weight: bold;");
    header->setMinimumHeight(66);
    reset_button_->setStyleSheet("background-color: #e1ad42; color: #202020; padding: 7px 17px;");
    exit_button_->setStyleSheet("background-color: #376fac; color: white; padding: 7px 17px;");

    auto* inspector_box = new QGroupBox("Term Inspector", central);
    auto* inspector_layout = new QVBoxLayout(inspector_box);
    inspector_layout->setContentsMargins(0, 0, 0, 0);
    inspector_layout->addWidget(term_inspector_widget_);

    auto* context_box = new QGroupBox("Context", central);
    auto* context_layout = new QVBoxLayout(context_box);
    context_layout->setContentsMargins(0, 0, 0, 0);
    context_layout->addWidget(context_widget_);

    auto* bottom_content = new QHBoxLayout;
    bottom_content->addWidget(inspector_box, 1);
    bottom_content->addWidget(context_box, 1);

    auto* footer = new QHBoxLayout;
    footer->addWidget(new QLabel("Current status:", central));
    footer->addWidget(status_label_, 1);
    footer->addWidget(reset_button_);
    footer->addWidget(exit_button_);

    auto* main_layout = new QVBoxLayout(central);
    main_layout->setContentsMargins(10, 10, 10, 10);
    main_layout->setSpacing(7);
    main_layout->addWidget(header);
    main_layout->addWidget(workspace_widget_, 4);
    main_layout->addWidget(search_widget_, 5);
    main_layout->addLayout(bottom_content, 3);
    main_layout->addLayout(footer);

    connect(workspace_widget_, &WorkspaceWidget::add_document_requested,
            this, &MainWindow::add_document);
    connect(workspace_widget_, &WorkspaceWidget::build_corpus_requested,
            this, &MainWindow::build_corpus);
    connect(workspace_widget_, &WorkspaceWidget::document_selected,
            this, &MainWindow::document_selected);
    connect(search_widget_, &SearchWidget::search_requested,
            this, &MainWindow::search_requested);
    connect(search_widget_, &SearchWidget::query_changed,
            this, &MainWindow::query_changed);
    connect(search_widget_, &SearchWidget::source_document_requested,
            this, &MainWindow::source_document_requested);
    connect(search_widget_, &SearchWidget::selected_chunk_changed,
            term_inspector_widget_, &TermInspectorWidget::set_selected_chunk);
    connect(term_inspector_widget_, &TermInspectorWidget::term_inspection_requested,
            this, &MainWindow::term_inspection_requested);
    connect(context_widget_, &ContextWidget::build_context_requested,
            this, &MainWindow::build_context_requested);
    connect(reset_button_, &QPushButton::clicked,
            this, &MainWindow::reset_application);
    connect(exit_button_, &QPushButton::clicked,
            this, &MainWindow::exit_application);

    update_action_states();
}

MainWindow::~MainWindow() {
    ipc_server_.stop();
    // Every state uses the same worker shutdown. This joins before the
    // window and its widgets are destroyed.
    state_->exit(*this);
    notification_watcher_->setEnabled(false);
    ::close(notification_read_fd_);
    ::close(notification_write_fd_);
    notification_read_fd_ = -1;
    notification_write_fd_ = -1;
}

void MainWindow::set_build_delay_ms(int ms_per_document) {
    build_delay_ms_ = std::max(0, ms_per_document);
}

void MainWindow::set_build_factory(aiws::AsyncCorpusBuild::CoreFactory factory) {
    builder_.set_factory(std::move(factory));
}

bool MainWindow::build_worker_active() const {
    return builder_.active();
}

bool MainWindow::build_worker_waiting() const {
    return builder_.waiting();
}

bool MainWindow::start_ipc_server(const QString& socket_path, QString* error) {
    std::string problem;
    const bool success = ipc_server_.start(socket_path.toUtf8().toStdString(),
        [this](const std::string& file) {
            // Save the path first. The GUI reads it after the pipe wakes it.
            {
                std::lock_guard<std::mutex> lock(remote_files_mutex_);
                remote_files_.push_back(file);
            }
            notify_gui();
        }, problem);
    if (error) *error = QString::fromUtf8(problem.c_str());
    return success;
}

void MainWindow::notify_gui() noexcept {
    // One byte is enough. If the pipe is full, it is already readable.
    const char byte = 1;
    ssize_t n;
    do {
        n = ::write(notification_write_fd_, &byte, 1);
    } while (n < 0 && errno == EINTR);
}

void MainWindow::handle_notifications() {
    // Drain the nonblocking pipe before handling the work it represents.
    char bytes[128];
    for (;;) {
        const ssize_t n = ::read(notification_read_fd_, bytes, sizeof(bytes));
        if (n > 0) continue;
        if (n < 0 && errno == EINTR) continue;
        break;
    }

    std::deque<std::string> files;
    {
        std::lock_guard<std::mutex> lock(remote_files_mutex_);
        files.swap(remote_files_);
    }
    for (const auto& file : files) {
        remote_document_arrived(QString::fromUtf8(file.data(),
                                  static_cast<int>(file.size())));
    }
    poll_build_completion();
}

void MainWindow::remote_document_arrived(const QString& path) {
    if (!add_document_file(path)) {
        status_label_->setText("Could not add remote document: file is no longer readable.");
    }
}

void MainWindow::set_exit_callback(ExitCallback callback) noexcept {
    exit_callback_ = callback;
}

void MainWindow::add_document() {
    const QString chosen = QFileDialog::getOpenFileName(
        this, "Add Document", QString(), "Text files (*.txt);;All files (*)");
    if (!chosen.isEmpty() && !add_document_file(chosen)) {
        status_label_->setText("Could not read the selected document.");
    }
}

bool MainWindow::add_document_file(const QString& path) {
    const QFileInfo info(path);
    const QString absolute = info.absoluteFilePath();
    QFile file(absolute);
    if (!info.isFile() || !file.open(QIODevice::ReadOnly)) return false;
    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) return false;

    for (const auto& document : workspace_.documents()) {
        if (QString::fromUtf8(document.id().c_str()) == absolute) {
            status_label_->setText("This document is already in the workspace.");
            return true;
        }
    }
    const QString contents = QString::fromUtf8(bytes.constData(), bytes.size());
    workspace_.add_document(Document(absolute.toUtf8().toStdString(),
                                     info.fileName().toUtf8().toStdString(),
                                     contents.toUtf8().toStdString()));
    ++workspace_revision_;
    if (auto next = state_->workspace_changed(*this)) state_ = std::move(next);
    update_action_states();
    workspace_widget_->add_document(info.fileName());
    status_label_->setText("Added " + info.fileName() + ". Build the corpus to search.");
    return true;
}

void MainWindow::build_corpus() {
    if (auto next = state_->build_requested(*this)) state_ = std::move(next);
    update_action_states();
}

void MainWindow::poll_build_completion() {
    aiws::AsyncCorpusBuild::Result result;
    while (builder_.try_take_result(result)) {
        if (auto next = state_->build_finished(*this, result)) {
            state_ = std::move(next);
        }
        update_action_states();
    }
}

void MainWindow::document_selected(int row) {
    const auto& documents = workspace_.documents();
    if (row < 0 || static_cast<std::size_t>(row) >= documents.size()) {
        workspace_widget_->clear_selected_document();
        return;
    }

    const auto& document = documents[static_cast<std::size_t>(row)];
    workspace_widget_->show_selected_document(
        QString::fromStdString(document.title()),
        QString::fromStdString(document.text()));
}

void MainWindow::search_requested(const QString& query, int k) {
    if (!state_->can_process() || query.trimmed().isEmpty()) return;

    try {
        auto results = processing_core_.search(query.toStdString(), k);
        const auto count = results.size();
        search_widget_->set_results(std::move(results));
        term_inspector_widget_->clear_statistics();
        context_widget_->clear_context();
        status_label_->setText(QString("Search complete: %1 results.").arg(static_cast<qulonglong>(count)));
    } catch (const std::exception& error) {
        status_label_->setText("Search failed: " + QString::fromUtf8(error.what()));
    }
}

void MainWindow::query_changed(const QString& query) {
    (void)query;
    update_action_states();
}

void MainWindow::build_context_requested(int token_budget) {
    if (!state_->can_process() || !search_widget_->has_query()) return;

    try {
        const auto items = processing_core_.build_context(
            search_widget_->query().toStdString(), search_widget_->result_count(),
            static_cast<std::size_t>(token_budget));
        context_widget_->set_context(items);
        status_label_->setText(QString("Context built: %1 chunks.").arg(static_cast<qulonglong>(items.size())));
    } catch (const std::exception& error) {
        status_label_->setText("Context failed: " + QString::fromUtf8(error.what()));
    }
}

void MainWindow::source_document_requested(const QString& document_id) {
    const auto& documents = workspace_.documents();
    for (std::size_t i = 0; i < documents.size(); ++i) {
        if (QString::fromStdString(documents[i].id()) == document_id) {
            workspace_widget_->select_document(static_cast<int>(i));
            document_selected(static_cast<int>(i));
            return;
        }
    }
    status_label_->setText("Source document not found.");
}

void MainWindow::term_inspection_requested(const QString& term,
                                           const QString& chunk_id) {
    if (!state_->can_process() || term.trimmed().isEmpty()) return;

    try {
        const auto df = processing_core_.document_frequency(term.toStdString());
        const bool has_chunk = !chunk_id.isEmpty();
        const auto tf = has_chunk
            ? processing_core_.term_frequency(term.toStdString(), chunk_id.toStdString()) : 0;
        term_inspector_widget_->set_statistics(df, has_chunk, tf);
        status_label_->setText("Term inspection complete.");
    } catch (const std::exception& error) {
        term_inspector_widget_->clear_statistics();
        status_label_->setText("Inspection failed: " + QString::fromUtf8(error.what()));
    }
}

void MainWindow::reset_application() {
    workspace_.clear();
    ++workspace_revision_;
    if (auto next = state_->reset(*this)) state_ = std::move(next);

    std::vector<Resettable*> components{
        workspace_widget_, search_widget_, term_inspector_widget_, context_widget_
    };
    for (auto* component : components) component->reset();

    update_action_states();
    status_label_->setText("Application reset.");
}

void MainWindow::exit_application() {
    if (exit_callback_) exit_callback_(this);
}

void MainWindow::update_action_states() {
    const bool processing_available = state_->can_process();
    const bool query_ready = processing_available && search_widget_->has_query();

    workspace_widget_->set_build_corpus_enabled(state_->can_build());
    search_widget_->set_search_enabled(query_ready);
    term_inspector_widget_->set_inspection_enabled(processing_available);
    context_widget_->set_build_context_enabled(query_ready);
}

void MainWindow::invalidate_corpus() {
    processing_core_ = ProcessingCore();
    workspace_widget_->set_corpus_status("Not built");
    clear_processing_outputs();
}

void MainWindow::clear_processing_outputs() {
    search_widget_->clear_results();
    term_inspector_widget_->set_selected_chunk(QString());
    term_inspector_widget_->clear_statistics();
    context_widget_->clear_context();
}

}  // namespace aiws::gui
