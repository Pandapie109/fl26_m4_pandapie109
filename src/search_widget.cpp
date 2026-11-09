#include "aiws/gui/search_widget.hpp"

#include "aiws/gui/search_result_model.hpp"

#include <QAbstractItemView>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableView>
#include <QVBoxLayout>

#include <utility>

namespace aiws::gui {

SearchWidget::SearchWidget(QWidget* parent) : QWidget(parent) {
    query_edit_ = new QLineEdit(this);
    query_edit_->setPlaceholderText("Search the current corpus");
    result_count_spin_ = new QSpinBox(this);
    result_count_spin_->setRange(1, 100);
    result_count_spin_->setValue(3);
    search_button_ = new QPushButton("Search", this);
    result_model_ = new SearchResultModel(this);
    results_view_ = new QTableView(this);
    results_view_->setModel(result_model_);
    results_view_->setSelectionBehavior(QAbstractItemView::SelectRows);
    results_view_->setSelectionMode(QAbstractItemView::SingleSelection);
    results_view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    results_view_->setWordWrap(false);
    results_view_->verticalHeader()->hide();
    results_view_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    results_view_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
    results_view_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    results_view_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    results_view_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    results_view_->setColumnWidth(1, 180);

    selected_result_text_ = new QPlainTextEdit(this);
    selected_result_text_->setReadOnly(true);
    selected_result_text_->setPlaceholderText("Select a result to inspect its details.");
    view_source_button_ = new QPushButton("View Source", this);

    auto* query_controls = new QHBoxLayout;
    query_controls->addWidget(new QLabel("Query:", this));
    query_controls->addWidget(query_edit_, 1);
    query_controls->addWidget(new QLabel("Results:", this));
    query_controls->addWidget(result_count_spin_);
    query_controls->addWidget(search_button_);

    auto* selection_controls = new QHBoxLayout;
    selection_controls->addWidget(new QLabel("Selected Result:", this));
    selection_controls->addStretch();
    selection_controls->addWidget(view_source_button_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->addLayout(query_controls);
    layout->addWidget(results_view_, 3);
    layout->addLayout(selection_controls);
    layout->addWidget(selected_result_text_, 2);

    connect(query_edit_, &QLineEdit::textChanged,
            this, &SearchWidget::query_changed);
    connect(search_button_, &QPushButton::clicked,
            this, &SearchWidget::request_search);
    connect(view_source_button_, &QPushButton::clicked,
            this, &SearchWidget::request_source_document);
    connect(results_view_->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &SearchWidget::show_selected_result);

    search_button_->setEnabled(false);
    view_source_button_->setEnabled(false);
}

QString SearchWidget::query() const {
    return query_edit_->text();
}

int SearchWidget::result_count() const {
    return result_count_spin_->value();
}

bool SearchWidget::has_query() const {
    return !query_edit_->text().trimmed().isEmpty();
}

void SearchWidget::set_search_enabled(bool enabled) {
    search_button_->setEnabled(enabled);
}

void SearchWidget::set_results(std::vector<SearchResult> results) {
    results_view_->clearSelection();
    result_model_->set_results(std::move(results));
    selected_result_text_->clear();
    view_source_button_->setEnabled(false);
    emit selected_chunk_changed(QString());
}

void SearchWidget::clear_results() {
    results_view_->clearSelection();
    result_model_->clear();
    selected_result_text_->clear();
    view_source_button_->setEnabled(false);
    emit selected_chunk_changed(QString());
}

void SearchWidget::reset() {
    query_edit_->clear();
    result_count_spin_->setValue(3);
    clear_results();
    set_search_enabled(false);
}

void SearchWidget::request_search() {
    if (!has_query()) return;
    emit search_requested(query(), result_count());
}

void SearchWidget::show_selected_result() {
    const QModelIndexList rows = results_view_->selectionModel()->selectedRows();
    const auto* result = rows.isEmpty() ? nullptr : result_model_->result_at(rows.first().row());
    if (!result) {
        selected_result_text_->clear();
        view_source_button_->setEnabled(false);
        emit selected_chunk_changed(QString());
        return;
    }

    selected_result_text_->setPlainText(
        QString("Rank: %1\nDocument: %2\nChunk: %3\nScore: %4\nText: %5")
            .arg(rows.first().row() + 1)
            .arg(QString::fromStdString(result->document_id))
            .arg(static_cast<qulonglong>(result->chunk_sequence))
            .arg(result->score, 0, 'f', 6)
            .arg(QString::fromStdString(result->text)));
    view_source_button_->setEnabled(true);
    emit selected_chunk_changed(QString::fromStdString(result->chunk_id));
}

void SearchWidget::request_source_document() {
    const QModelIndexList rows = results_view_->selectionModel()->selectedRows();
    if (rows.isEmpty()) return;
    const auto* result = result_model_->result_at(rows.first().row());
    if (result) emit source_document_requested(QString::fromStdString(result->document_id));
}

}  // namespace aiws::gui
