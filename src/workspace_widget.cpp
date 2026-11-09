#include "aiws/gui/workspace_widget.hpp"

#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace aiws::gui {

WorkspaceWidget::WorkspaceWidget(QWidget* parent) : QWidget(parent) {
    document_list_ = new QListWidget(this);
    document_list_->setObjectName("documentList");
    add_document_button_ = new QPushButton("Add Document", this);
    build_corpus_button_ = new QPushButton("Build Corpus", this);
    build_corpus_button_->setObjectName("buildCorpusButton");
    corpus_status_label_ = new QLabel("Not built", this);
    corpus_status_label_->setObjectName("corpusStatus");
    document_title_label_ = new QLabel("Selected Document -", this);
    document_text_ = new QPlainTextEdit(this);
    document_text_->setReadOnly(true);
    document_text_->setPlaceholderText("Select a document to view its contents.");

    auto* workspace_group = new QGroupBox("Workspace", this);
    auto* document_group = new QGroupBox("Selected Document", this);

    auto* controls = new QHBoxLayout;
    controls->addWidget(add_document_button_);
    controls->addWidget(build_corpus_button_);
    controls->addStretch();

    auto* corpus_line = new QHBoxLayout;
    corpus_line->addWidget(new QLabel("Corpus:", this));
    corpus_line->addWidget(corpus_status_label_);
    corpus_line->addStretch();

    auto* workspace_layout = new QVBoxLayout(workspace_group);
    workspace_layout->addLayout(controls);
    workspace_layout->addWidget(document_list_, 1);
    workspace_layout->addLayout(corpus_line);

    auto* document_layout = new QVBoxLayout(document_group);
    document_layout->addWidget(document_title_label_);
    document_layout->addWidget(document_text_, 1);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(workspace_group, 1);
    layout->addWidget(document_group, 1);

    connect(add_document_button_, &QPushButton::clicked,
            this, &WorkspaceWidget::add_document_requested);
    connect(build_corpus_button_, &QPushButton::clicked,
            this, &WorkspaceWidget::build_corpus_requested);
    connect(document_list_, &QListWidget::currentRowChanged,
            this, &WorkspaceWidget::document_selected);

    build_corpus_button_->setEnabled(false);
}

void WorkspaceWidget::add_document(const QString& title) {
    document_list_->addItem(title);
    document_list_->setCurrentRow(document_list_->count() - 1);
}

void WorkspaceWidget::select_document(int row) {
    if (row < 0 || row >= document_list_->count()) return;
    document_list_->setCurrentRow(row);
}

void WorkspaceWidget::show_selected_document(const QString& title,
                                              const QString& text) {
    document_title_label_->setText("Selected Document - " + title);
    document_text_->setPlainText(text);
}

void WorkspaceWidget::clear_selected_document() {
    document_title_label_->setText("Selected Document -");
    document_text_->clear();
}

void WorkspaceWidget::set_corpus_status(const QString& text) {
    corpus_status_label_->setText(text);
}

void WorkspaceWidget::set_build_corpus_enabled(bool enabled) {
    build_corpus_button_->setEnabled(enabled);
}

void WorkspaceWidget::reset() {
    document_list_->clear();
    clear_selected_document();
    set_corpus_status("Not built");
    set_build_corpus_enabled(false);
}

}  // namespace aiws::gui
