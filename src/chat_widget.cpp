/**
 * @file chat_widget.cpp
 * @brief Discord-tier chat UI implementation
 *
 * Custom QPainter delegate renders each message as a styled card with:
 * - Left accent bar (purple=AI, green=User)
 * - Sender name in bold
 * - Word-wrapped text via QTextDocument
 * - Rounded corners via QPainterPath
 * - Hover highlight
 */

#include "chat_widget.h"

#include <QVBoxLayout>
#include <QScrollBar>
#include <QTextDocument>
#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QClipboard>
#include <QMouseEvent>
#include <QPainterPath>

namespace vision {

// ═══════════════════ Model ══════════════════════════════════════════

ChatMessageModel::ChatMessageModel(QObject* parent)
    : QAbstractListModel(parent) {}

int ChatMessageModel::rowCount(const QModelIndex&) const {
    return static_cast<int>(messages_.size());
}

QVariant ChatMessageModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= (int)messages_.size())
        return {};

    const auto& msg = messages_[index.row()];
    switch (role) {
        case SenderRole:    return msg.sender;
        case TextRole:      return msg.text;
        case TimestampRole: return msg.timestamp;
        case TypeRole:      return static_cast<int>(msg.type);
        case CopiedStateRole: return (index.row() < (int)copied_states_.size())
                                     ? copied_states_[index.row()] : false;
        case Qt::DisplayRole: return msg.text;
        default: return {};
    }
}

bool ChatMessageModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    if (!index.isValid() || index.row() >= (int)messages_.size())
        return false;
    if (role == CopiedStateRole) {
        if (index.row() < (int)copied_states_.size()) {
            copied_states_[index.row()] = value.toBool();
            emit dataChanged(index, index, {CopiedStateRole});
            return true;
        }
    }
    return false;
}

Qt::ItemFlags ChatMessageModel::flags(const QModelIndex& index) const {
    if (!index.isValid()) return Qt::NoItemFlags;
    return Qt::ItemIsEnabled | Qt::ItemIsEditable;  // Editable enables editorEvent
}

void ChatMessageModel::addMessage(const QString& sender, const QString& text,
                                   MessageType type) {
    int row = static_cast<int>(messages_.size());
    beginInsertRows(QModelIndex(), row, row);
    messages_.push_back({sender, text, QDateTime::currentDateTime(), type});
    copied_states_.push_back(false);
    endInsertRows();
}

void ChatMessageModel::clear() {
    if (messages_.empty()) return;
    beginResetModel();
    messages_.clear();
    copied_states_.clear();
    endResetModel();
}

void ChatMessageModel::streamToken(const QString& token) {
    if (messages_.empty()) return; // Nowhere to stream to

    int last_idx = static_cast<int>(messages_.size()) - 1;
    messages_[last_idx].text += token;

    // Tell the view that exactly this one item changed
    QModelIndex idx = index(last_idx, 0);
    emit dataChanged(idx, idx, {TextRole, Qt::DisplayRole});
}

// ═══════════════════ Delegate ═══════════════════════════════════════

ChatMessageDelegate::ChatMessageDelegate(QObject* parent)
    : QStyledItemDelegate(parent) {}

QFont ChatMessageDelegate::senderFont() const {
    QFont f("Inter", 11);
    if (!QFontInfo(f).exactMatch()) f.setFamily("Segoe UI");
    f.setBold(true);
    return f;
}

QFont ChatMessageDelegate::messageFont() const {
    QFont f("Inter", 10);
    if (!QFontInfo(f).exactMatch()) f.setFamily("Segoe UI");
    return f;
}

QFont ChatMessageDelegate::timestampFont() const {
    QFont f("Inter", 8);
    if (!QFontInfo(f).exactMatch()) f.setFamily("Segoe UI");
    f.setWeight(QFont::Light);
    return f;
}

int ChatMessageDelegate::bubbleTextWidth(const QStyleOptionViewItem& option) const {
    int viewWidth = option.rect.width();
    int maxBubble = static_cast<int>(viewWidth * kMaxBubbleRatio);
    return maxBubble - (kBubblePadding * 2) - kAccentBarWidth - kTimestampWidth;
}

int ChatMessageDelegate::calcTextHeight(const QString& text, int width) const {
    QTextDocument doc;
    doc.setDefaultFont(messageFont());
    doc.setTextWidth(width);
    doc.setPlainText(text);
    return static_cast<int>(doc.size().height());
}

// ═══ SHARED GEOMETRY — used by BOTH paint() and editorEvent() ═══
// If these go out of sync, clicks won't match the drawn icon!
QRect ChatMessageDelegate::getCopyButtonRect(const QRect& bubbleRect) const {
    return QRect(
        bubbleRect.right() - kBubblePadding - kCopyBtnSize,
        bubbleRect.top() + 4,
        kCopyBtnSize, kCopyBtnSize
    );
}

bool ChatMessageDelegate::editorEvent(QEvent* event, QAbstractItemModel* model,
                                       const QStyleOptionViewItem& option,
                                       const QModelIndex& index) {
    if (event->type() != QEvent::MouseButtonPress)
        return false;

    auto* me = static_cast<QMouseEvent*>(event);

    // Reconstruct bubble rect (same math as paint)
    int viewWidth = option.rect.width();
    int maxBubbleWidth = static_cast<int>(viewWidth * kMaxBubbleRatio);
    auto type = static_cast<MessageType>(index.data(TypeRole).toInt());
    bool isUser = (type == MessageType::User);

    QString text = index.data(TextRole).toString();
    QTextDocument doc;
    doc.setDefaultFont(messageFont());
    doc.setTextWidth(bubbleTextWidth(option));
    doc.setPlainText(text);
    int actualTextWidth = static_cast<int>(doc.idealWidth()) + 1;
    int textHeight = static_cast<int>(doc.size().height());

    int bubbleContentWidth = std::min(actualTextWidth + kAccentBarWidth + kTimestampWidth,
                                       maxBubbleWidth - kBubblePadding * 2);
    int totalBubbleWidth = bubbleContentWidth + kBubblePadding * 2;
    int bubbleX = isUser ? (viewWidth - totalBubbleWidth - kBubbleMarginH)
                         : kBubbleMarginH;
    int bubbleY = option.rect.y() + kBubbleMarginV;
    int bubbleHeight = kSenderHeight + kBubblePadding + textHeight + kBubblePadding;
    QRect bubbleRect(bubbleX, bubbleY, totalBubbleWidth, bubbleHeight);

    // Check if click is inside copy button area
    QRect copyBtn = getCopyButtonRect(bubbleRect);
    if (copyBtn.contains(me->pos())) {
        // Copy to clipboard
        QApplication::clipboard()->setText(text);

        // Set "Copied ✓" state
        model->setData(index, true, CopiedStateRole);

        // Reset after 2 seconds
        QPersistentModelIndex pIdx(index);
        QTimer::singleShot(2000, [model, pIdx]() {
            if (pIdx.isValid()) {
                model->setData(pIdx, false, CopiedStateRole);
            }
        });

        return true;  // Event consumed
    }
    return false;
}

QSize ChatMessageDelegate::sizeHint(const QStyleOptionViewItem& option,
                                      const QModelIndex& index) const {
    QString text = index.data(TextRole).toString();
    int textWidth = bubbleTextWidth(option);
    int textHeight = calcTextHeight(text, textWidth);

    // Total height = top margin + sender line + padding + text + padding + bottom margin
    int totalHeight = kBubbleMarginV + kSenderHeight + kBubblePadding + textHeight
                    + kBubblePadding + kBubbleMarginV;

    return QSize(option.rect.width(), totalHeight);
}

void ChatMessageDelegate::paint(QPainter* painter,
                                  const QStyleOptionViewItem& option,
                                  const QModelIndex& index) const {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    // ── Extract data ────────────────────────────────────────────
    QString sender = index.data(SenderRole).toString();
    QString text = index.data(TextRole).toString();
    QDateTime timestamp = index.data(TimestampRole).toDateTime();
    auto type = static_cast<MessageType>(index.data(TypeRole).toInt());

    bool isUser = (type == MessageType::User);
    bool isSystem = (type == MessageType::System);
    bool isHovered = option.state & QStyle::State_MouseOver;

    // ── Colors ──────────────────────────────────────────────────
    QColor bubbleBg = isSystem ? bgSystem_ : (isUser ? bgUser_ : bgAI_);
    QColor accent = isUser ? accentUser_ : accentAI_;
    QColor senderColor = isUser ? QColor(255, 255, 255) : accentAI_;
    if (isSystem) senderColor = dimText_;
    if (isHovered && !isSystem) bubbleBg = hoverBg_;

    // ── Geometry ────────────────────────────────────────────────
    int viewWidth = option.rect.width();
    int maxBubbleWidth = static_cast<int>(viewWidth * kMaxBubbleRatio);

    // Calculate actual text width needed (may be less than max)
    QTextDocument doc;
    doc.setDefaultFont(messageFont());
    doc.setTextWidth(bubbleTextWidth(option));
    doc.setPlainText(text);
    int actualTextWidth = static_cast<int>(doc.idealWidth()) + 1;
    int textHeight = static_cast<int>(doc.size().height());

    // Bubble width: fit content but don't exceed max
    int bubbleContentWidth = std::min(actualTextWidth + kAccentBarWidth + kTimestampWidth,
                                       maxBubbleWidth - kBubblePadding * 2);
    int totalBubbleWidth = bubbleContentWidth + kBubblePadding * 2;

    // Position: user right-aligned, AI left-aligned
    int bubbleX = isUser ? (viewWidth - totalBubbleWidth - kBubbleMarginH)
                         : kBubbleMarginH;
    int bubbleY = option.rect.y() + kBubbleMarginV;
    int bubbleHeight = kSenderHeight + kBubblePadding + textHeight + kBubblePadding;

    QRect bubbleRect(bubbleX, bubbleY, totalBubbleWidth, bubbleHeight);

    // ── Draw bubble background ──────────────────────────────────
    QPainterPath path;
    path.addRoundedRect(bubbleRect, kBubbleRadius, kBubbleRadius);
    painter->fillPath(path, bubbleBg);

    // ── Draw accent bar (left edge for AI, right edge for User) ─
    if (!isSystem) {
        QRect barRect;
        if (isUser) {
            barRect = QRect(bubbleRect.right() - kAccentBarWidth,
                           bubbleRect.y(), kAccentBarWidth, bubbleRect.height());
        } else {
            barRect = QRect(bubbleRect.x(), bubbleRect.y(),
                           kAccentBarWidth, bubbleRect.height());
        }
        // Clip to rounded rect
        painter->save();
        painter->setClipPath(path);
        painter->fillRect(barRect, accent);
        painter->restore();
    }

    // ── Draw sender name ────────────────────────────────────────
    int textStartX = bubbleRect.x() + kBubblePadding + (isUser ? 0 : kAccentBarWidth);
    int textAvailWidth = bubbleRect.width() - kBubblePadding * 2 - kAccentBarWidth;

    QRect senderRect(textStartX, bubbleY + 6, textAvailWidth - kTimestampWidth, kSenderHeight);
    painter->setFont(senderFont());
    painter->setPen(senderColor);
    painter->drawText(senderRect, Qt::AlignLeft | Qt::AlignVCenter, sender);

    // ── Draw timestamp ──────────────────────────────────────────
    QRect timeRect(bubbleRect.right() - kBubblePadding - kTimestampWidth - kCopyBtnSize - 4,
                   bubbleY + 6, kTimestampWidth, kSenderHeight);
    painter->setFont(timestampFont());
    painter->setPen(dimText_);
    QString timeStr = timestamp.toString("hh:mm");
    painter->drawText(timeRect, Qt::AlignRight | Qt::AlignVCenter, timeStr);

    // ── Draw copy button (hover only, or "Copied ✓" feedback) ────
    bool isCopied = index.data(CopiedStateRole).toBool();
    QRect copyBtn = getCopyButtonRect(bubbleRect);

    if (isCopied) {
        // Green "Copied ✓" feedback
        QFont copyFont = timestampFont();
        copyFont.setBold(true);
        painter->setFont(copyFont);
        painter->setPen(QColor(57, 255, 20));   // #39FF14 Neon Green
        painter->drawText(copyBtn, Qt::AlignCenter, "✓");
    } else if (isHovered) {
        // Show copy icon on hover
        painter->setFont(timestampFont());
        painter->setPen(dimText_);
        painter->drawText(copyBtn, Qt::AlignCenter, "📋");
    }

    // ── Draw message text (with proper word-wrap) ───────────────
    // Use QTextDocument for pixel-perfect word-wrap rendering
    QRect textRect(textStartX,
                   bubbleY + kSenderHeight + kBubblePadding - 2,
                   textAvailWidth, textHeight);

    painter->save();
    painter->translate(textRect.topLeft());
    painter->setPen(textColor_);

    QTextDocument renderDoc;
    renderDoc.setDefaultFont(messageFont());
    renderDoc.setTextWidth(textAvailWidth);
    renderDoc.setPlainText(text);

    // Set default text color
    QAbstractTextDocumentLayout::PaintContext ctx;
    ctx.palette.setColor(QPalette::Text, textColor_);
    renderDoc.documentLayout()->draw(painter, ctx);

    painter->restore();

    painter->restore();
}

// ═══════════════════ Widget ═════════════════════════════════════════

ChatWidget::ChatWidget(QWidget* parent) : QWidget(parent) {
    model_ = new ChatMessageModel(this);
    delegate_ = new ChatMessageDelegate(this);
    view_ = new QListView(this);

    view_->setModel(model_);
    view_->setItemDelegate(delegate_);

    // Performance & behavior
    view_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    view_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    view_->setSelectionMode(QAbstractItemView::NoSelection);
    view_->setMouseTracking(true);  // Enable hover effects in delegate
    view_->setUniformItemSizes(false);  // Each bubble has different height
    view_->setSpacing(0);  // We handle spacing in the delegate
    view_->setFocusPolicy(Qt::NoFocus);  // Don't steal focus from input

    // Smooth scrolling
    view_->verticalScrollBar()->setSingleStep(20);

    // Layout
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(view_);

    setupStyle();

    // Resize triggers sizeHint recalculation for word-wrap
    connect(model_, &QAbstractListModel::rowsInserted, this, &ChatWidget::scrollToBottom);
}

void ChatWidget::setupStyle() {
    view_->setStyleSheet(
        "QListView {"
        "  background-color: #1e1f22;"
        "  border: 1px solid #2b2d31;"
        "  border-radius: 10px;"
        "  outline: none;"
        "}"
        // ── Premium thin scrollbar ──
        "QScrollBar:vertical {"
        "  background-color: transparent;"
        "  width: 8px;"
        "  margin: 4px 2px 4px 0px;"
        "}"
        "QScrollBar::handle:vertical {"
        "  background-color: #3f3f46;"
        "  min-height: 30px;"
        "  border-radius: 4px;"
        "}"
        "QScrollBar::handle:vertical:hover {"
        "  background-color: #5865F2;"
        "}"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {"
        "  height: 0px;"
        "}"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {"
        "  background: transparent;"
        "}"
    );
}

void ChatWidget::addMessage(const QString& sender, const QString& text,
                             MessageType type) {
    model_->addMessage(sender, text, type);
}

void ChatWidget::clear() {
    model_->clear();
}

void ChatWidget::streamToken(const QString& token) {
    model_->streamToken(token);
    scrollToBottom();
}

int ChatWidget::messageCount() const {
    return model_->rowCount();
}

void ChatWidget::scrollToBottom() {
    // Defer scroll so the view has time to layout the new item
    QTimer::singleShot(10, this, [this]() {
        view_->scrollToBottom();
    });
}

} // namespace vision
