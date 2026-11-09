#include "aiws/gui/context_widget.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSlider>
#include <QStringList>
#include <QVBoxLayout>

namespace aiws::gui {

ContextWidget::ContextWidget(QWidget* parent) : QWidget(parent) {
    context_budget_slider_ = new QSlider(Qt::Horizontal, this);
    context_budget_slider_->setRange(50, 1000);
    context_budget_slider_->setSingleStep(50);
    context_budget_slider_->setPageStep(50);
    context_budget_slider_->setTickInterval(50);
    context_budget_slider_->setTickPosition(QSlider::TicksBelow);
    context_budget_slider_->setValue(300);

    budget_value_label_ = new QLabel("300", this);
    build_context_button_ = new QPushButton("Build Context", this);
    context_text_ = new QPlainTextEdit(this);
    context_text_->setReadOnly(true);
    context_text_->setPlaceholderText("Built context will appear here.");

    auto* controls = new QHBoxLayout;
    controls->addWidget(new QLabel("Token budget:", this));
    controls->addWidget(context_budget_slider_, 1);
    controls->addWidget(budget_value_label_);
    controls->addWidget(build_context_button_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->addLayout(controls);
    layout->addWidget(context_text_, 1);

    connect(context_budget_slider_, &QSlider::valueChanged,
            this, &ContextWidget::budget_changed);
    connect(build_context_button_, &QPushButton::clicked,
            this, &ContextWidget::request_context);
    build_context_button_->setEnabled(false);
}

int ContextWidget::token_budget() const {
    return context_budget_slider_->value();
}

void ContextWidget::set_build_context_enabled(bool enabled) {
    build_context_button_->setEnabled(enabled);
}

void ContextWidget::set_context(const std::vector<ContextItem>& items) {
    QStringList sections;
    for (const auto& item : items) {
        sections << QString("Document: %1\nChunk: %2\nTokens: %3\nScore: %4\nTruncated: %5\nText: %6")
            .arg(QString::fromStdString(item.document_id))
            .arg(static_cast<qulonglong>(item.chunk_sequence))
            .arg(static_cast<qulonglong>(item.token_count))
            .arg(item.score, 0, 'f', 6)
            .arg(item.truncated ? "Yes" : "No")
            .arg(QString::fromStdString(item.text));
    }
    context_text_->setPlainText(sections.join("\n\n"));
}

void ContextWidget::clear_context() {
    context_text_->clear();
}

void ContextWidget::reset() {
    context_budget_slider_->setValue(300);
    clear_context();
    set_build_context_enabled(false);
}

void ContextWidget::request_context() {
    emit build_context_requested(token_budget());
}

void ContextWidget::budget_changed(int value) {
    const int step_value = ((value + 25) / 50) * 50;
    if (step_value != value) {
        context_budget_slider_->setValue(step_value);
        return;
    }
    budget_value_label_->setText(QString::number(value));
}

}  // namespace aiws::gui
