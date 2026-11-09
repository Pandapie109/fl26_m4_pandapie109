#include "aiws/gui/search_result_model.hpp"

#include <QString>

#include <utility>

namespace aiws::gui {

SearchResultModel::SearchResultModel(QObject* parent)
    : QAbstractTableModel(parent) {}

int SearchResultModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(results_.size());
}

int SearchResultModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : 5;
}

QVariant SearchResultModel::data(const QModelIndex& index, int role) const {
    if (role != Qt::DisplayRole || !index.isValid() ||
        index.column() < 0 || index.column() >= 5) {
        return {};
    }

    const auto* result = result_at(index.row());
    if (!result) return {};

    switch (index.column()) {
    case 0: return index.row() + 1;
    case 1: return QString::fromStdString(result->document_id);
    case 2: return static_cast<qulonglong>(result->chunk_sequence);
    case 3: return QString::number(result->score, 'f', 6);
    case 4: return QString::fromStdString(result->text);
    default: return {};
    }
}

QVariant SearchResultModel::headerData(int section,
                                       Qt::Orientation orientation,
                                       int role) const {
    if (role != Qt::DisplayRole) return {};
    if (orientation == Qt::Vertical) return section + 1;

    switch (section) {
    case 0: return QStringLiteral("Rank");
    case 1: return QStringLiteral("Document");
    case 2: return QStringLiteral("Chunk");
    case 3: return QStringLiteral("Score");
    case 4: return QStringLiteral("Text");
    default: return {};
    }
}

void SearchResultModel::set_results(std::vector<SearchResult> results) {
    beginResetModel();
    results_ = std::move(results);
    endResetModel();
}

void SearchResultModel::clear() {
    beginResetModel();
    results_.clear();
    endResetModel();
}

const SearchResult* SearchResultModel::result_at(int row) const noexcept {
    if (row < 0 || static_cast<std::size_t>(row) >= results_.size()) return nullptr;
    return &results_[static_cast<std::size_t>(row)];
}

}  // namespace aiws::gui
