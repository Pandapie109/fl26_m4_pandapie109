#include "aiws/gui/main_window.hpp"

#include "aiws/gui/context_widget.hpp"
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
#include <QTimer>
#include <QMetaObject>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>
#include <vector>

namespace aiws::gui {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
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

    build_timer_ = new QTimer(this);
    build_timer_->setInterval(20);
    connect(build_timer_, &QTimer::timeout, this, &MainWindow::poll_build_completion);
    build_timer_->start();
    update_action_states();
}

MainWindow::~MainWindow() {
    // Shutdown order: stop accepting external requests before QObject teardown.
    // The worker's destructor requests cancellation and joins (no detached threads).
    ipc_server_.stop();
    builder_.request_cancel();
}

void MainWindow::set_build_delay_ms(int ms_per_document) {
    build_delay_ms_ = std::max(0, ms_per_document);
}

void MainWindow::set_build_factory(aiws::AsyncCorpusBuild::CoreFactory factory) {
    builder_.set_factory(std::move(factory));
}

bool MainWindow::start_ipc_server(const QString& socket_path, QString* error) {
    std::string problem;
    const bool success = ipc_server_.start(socket_path.toUtf8().toStdString(),
        [this](const std::string& file) {
            // A queued method call does not wait for the GUI thread. The main
            // window is kept alive until after ipc_server_.stop() joins.
            const QString path = QString::fromUtf8(file.data(), static_cast<int>(file.size()));
            if (!QMetaObject::invokeMethod(this, "remote_document_arrived",
                      Qt::QueuedConnection, Q_ARG(QString, path))) {
                throw std::runtime_error("Could not queue GUI file addition");
            }
        }, problem);
    if (error) *error = QString::fromUtf8(problem.c_str());
    return success;
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
    invalidate_corpus();
    workspace_widget_->add_document(info.fileName());
    status_label_->setText("Added " + info.fileName() + ". Build the corpus to search.");
    return true;
}

void MainWindow::build_corpus() {
    if (workspace_.documents().empty() || build_in_progress_) return;

    // Never rebuild on the GUI thread. The worker takes an owned snapshot.
    invalidate_corpus();
    if (!builder_.start(workspace_, workspace_revision_, build_delay_ms_)) {
        status_label_->setText("Could not start corpus build.");
        return;
    }
    build_in_progress_ = true;
    workspace_widget_->set_corpus_status("Building...");
    status_label_->setText("Building corpus in background...");
    update_action_states();
}

void MainWindow::poll_build_completion() {
    if (!build_in_progress_) return;
    aiws::AsyncCorpusBuild::Result result;
    if (!builder_.try_take_result(result)) return;

#ifndef AIWS_FAULT_ACCEPT_STALE
    // A document add or Reset invalidates all older build completions.
    if (result.workspace_revision != workspace_revision_) {
        build_in_progress_ = false;
        update_action_states();
        status_label_->setText("Outdated corpus build discarded.");
        return;
    }
#endif
    if (!result.error.empty() || result.cancelled || !result.core) {
#ifdef AIWS_FAULT_LOSE_FAILURE
        // Intentional fault: drop the error event and remain in Building.
        return;
#else
        build_in_progress_ = false;
        invalidate_corpus();
        status_label_->setText("Corpus build failed: " +
                               QString::fromUtf8(result.error.empty()
                                ? "Build cancelled" : result.error.c_str()));
        update_action_states();
        return;
#endif
    }
    // The completed core is now GUI-thread-owned. No worker accesses it.
    processing_core_ = std::move(*result.core);
    build_in_progress_ = false;
    corpus_current_ = true;
    clear_processing_outputs();
    workspace_widget_->set_corpus_status(
        QString::number(static_cast<qulonglong>(processing_core_.chunk_count())) + " chunks");
    status_label_->setText("Corpus built.");
    update_action_states();
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
    if (!corpus_current_ || query.trimmed().isEmpty()) return;

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
    if (!corpus_current_ || !search_widget_->has_query()) return;

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
    if (!corpus_current_ || term.trimmed().isEmpty()) return;

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
    ++workspace_revision_; // Reset invalidates an in-flight snapshot, even if empty.
    processing_core_ = ProcessingCore();
    corpus_current_ = false;

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
    const bool documents_available = !workspace_.documents().empty();
    const bool query_ready = corpus_current_ && search_widget_->has_query();

    workspace_widget_->set_build_corpus_enabled(documents_available && !build_in_progress_);
    search_widget_->set_search_enabled(query_ready);
    term_inspector_widget_->set_inspection_enabled(corpus_current_);
    context_widget_->set_build_context_enabled(query_ready);
}

void MainWindow::invalidate_corpus() {
    corpus_current_ = false;
    workspace_widget_->set_corpus_status("Not built");
    clear_processing_outputs();
    update_action_states();
}

void MainWindow::clear_processing_outputs() {
    search_widget_->clear_results();
    term_inspector_widget_->set_selected_chunk(QString());
    term_inspector_widget_->clear_statistics();
    context_widget_->clear_context();
}

}  // namespace aiws::gui
