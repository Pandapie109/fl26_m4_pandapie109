#include "aiws/gui/term_inspector_widget.hpp"

#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>

namespace aiws::gui {

TermInspectorWidget::TermInspectorWidget(QWidget* parent) : QWidget(parent) {
    term_edit_ = new QLineEdit(this);
    term_edit_->setPlaceholderText("Enter a term");
    inspect_button_ = new QPushButton("Inspect", this);
    document_frequency_value_ = new QLabel("—", this);
    chunk_frequency_value_ = new QLabel("—", this);

    auto* layout = new QGridLayout(this);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->addWidget(new QLabel("Term:", this), 0, 0);
    layout->addWidget(term_edit_, 0, 1);
    layout->addWidget(inspect_button_, 0, 2);
    layout->addWidget(new QLabel("Document Frequency:", this), 1, 0);
    layout->addWidget(document_frequency_value_, 1, 1);
    layout->addWidget(new QLabel("Selected Chunk TF:", this), 2, 0);
    layout->addWidget(chunk_frequency_value_, 2, 1);
    layout->setColumnStretch(1, 1);
    layout->setRowStretch(3, 1);

    connect(inspect_button_, &QPushButton::clicked,
            this, &TermInspectorWidget::request_inspection);
    inspect_button_->setEnabled(false);
}

void TermInspectorWidget::set_inspection_enabled(bool enabled) {
    inspect_button_->setEnabled(enabled);
}

void TermInspectorWidget::set_selected_chunk(const QString& chunk_id) {
    if (selected_chunk_id_ == chunk_id) return;
    selected_chunk_id_ = chunk_id;
    clear_statistics();
}

void TermInspectorWidget::set_statistics(std::size_t document_frequency,
                                         bool has_chunk_frequency,
                                         std::size_t chunk_frequency) {
    document_frequency_value_->setText(QString::number(static_cast<qulonglong>(document_frequency)));
    chunk_frequency_value_->setText(has_chunk_frequency
        ? QString::number(static_cast<qulonglong>(chunk_frequency)) : QStringLiteral("—"));
}

void TermInspectorWidget::clear_statistics() {
    document_frequency_value_->setText("—");
    chunk_frequency_value_->setText("—");
}

void TermInspectorWidget::reset() {
    term_edit_->clear();
    selected_chunk_id_.clear();
    clear_statistics();
    set_inspection_enabled(false);
}

void TermInspectorWidget::request_inspection() {
    const QString term = term_edit_->text().trimmed();
    if (term.isEmpty()) return;
    emit term_inspection_requested(term, selected_chunk_id_);
}

}  // namespace aiws::gui
