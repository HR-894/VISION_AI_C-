/**
 * @file chat_widget.cpp
 * @brief Modern conversational chat UI implementation
 *
 * Custom QPainter delegate renders each message as a sleek bubble:
 * - User: Vibrant blue bubble, right-aligned, white text
 * - AI:   Deep zinc bubble, left-aligned, light text with violet sender
 * - System: Subtle dark card, centered
 * - Smooth rounded corners (18px radius)
 * - Hover glow, copy-to-clipboard button
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
    return Qt::ItemIsEnabled | Qt::ItemIsEditable;
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
    if (messages_.empty()) return;

    int last_idx = static_cast<int>(messages_.size()) - 1;
    messages_[last_idx].text += token;

    QModelIndex idx = index(last_idx, 0);
    emit dataChanged(idx, idx, {TextRole, Qt::DisplayRole});
}

// ═══════════════════ Delegate ═══════════════════════════════════════

ChatMessageDelegate::ChatMessageDelegate(QObject* parent)
    : QStyledItemDelegate(parent) {
    
    add_anim_ = new QVariantAnimation(this);
    add_anim_->setStartValue(0.0f);
    add_anim_->setEndValue(1.0f);
    add_anim_->setDuration(400); // 400ms for snappy, fluid feel
    add_anim_->setEasingCurve(QEasingCurve::OutExpo);
    
    // Trigger repaint of the viewport on every animation frame
    connect(add_anim_, &QVariantAnimation::valueChanged, this, [this, parent](const QVariant&) {
        if (auto* view = qobject_cast<QAbstractItemView*>(parent)) {
            view->viewport()->update();
        }
    });
}

void ChatMessageDelegate::triggerNewMessageAnimation(int row) {
    animating_row_ = row;
    add_anim_->stop();
    add_anim_->start();
}

QFont ChatMessageDelegate::senderFont() const {
    QFont f("Segoe UI", 10);
    f.setWeight(QFont::DemiBold);
    return f;
}

QFont ChatMessageDelegate::messageFont() const {
    QFont f("Segoe UI", 10);
    f.setWeight(QFont::Normal);
    return f;
}

QFont ChatMessageDelegate::timestampFont() const {
    QFont f("Segoe UI", 8);
    f.setWeight(QFont::Light);
    return f;
}

int ChatMessageDelegate::bubbleTextWidth(const QStyleOptionViewItem& option) const {
    int viewWidth = option.rect.width();
    int maxBubble = static_cast<int>(viewWidth * kMaxBubbleRatio);
    return maxBubble - (kBubblePadding * 2) - kTimestampWidth;
}

int ChatMessageDelegate::calcTextHeight(const QString& text, int width) const {
    QTextDocument doc;
    doc.setDefaultFont(messageFont());
    doc.setTextWidth(width);
    doc.setPlainText(text);
    return static_cast<int>(doc.size().height());
}

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

    int bubbleContentWidth = std::min(actualTextWidth + kTimestampWidth,
                                       maxBubbleWidth - kBubblePadding * 2);
    int totalBubbleWidth = bubbleContentWidth + kBubblePadding * 2;
    int bubbleX = isUser ? (viewWidth - totalBubbleWidth - kBubbleMarginH)
                         : kBubbleMarginH;
    int bubbleY = option.rect.y() + kBubbleMarginV;
    int bubbleHeight = kSenderHeight + kBubblePadding + textHeight + kBubblePadding;
    QRect bubbleRect(bubbleX, bubbleY, totalBubbleWidth, bubbleHeight);

    QRect copyBtn = getCopyButtonRect(bubbleRect);
    if (copyBtn.contains(me->pos())) {
        QApplication::clipboard()->setText(text);
        model->setData(index, true, CopiedStateRole);

        QPersistentModelIndex pIdx(index);
        QTimer::singleShot(2000, [model, pIdx]() {
            if (pIdx.isValid()) {
                model->setData(pIdx, false, CopiedStateRole);
            }
        });
        return true;
    }
    return false;
}

QSize ChatMessageDelegate::sizeHint(const QStyleOptionViewItem& option,
                                      const QModelIndex& index) const {
    QString text = index.data(TextRole).toString();
    int textWidth = bubbleTextWidth(option);
    int textHeight = calcTextHeight(text, textWidth);

    int totalHeight = kBubbleMarginV + kSenderHeight + kBubblePadding + textHeight
                    + kBubblePadding + kBubbleMarginV;

    return QSize(option.rect.width(), totalHeight);
}

void ChatMessageDelegate::paint(QPainter* painter,
                                  const QStyleOptionViewItem& option,
                                  const QModelIndex& index) const {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::TextAntialiasing, true);

    // ── Kinetic Animation Math ──────────────────────────────────
    float anim_progress = 1.0f;
    if (index.row() == animating_row_ && add_anim_->state() == QAbstractAnimation::Running) {
        anim_progress = add_anim_->currentValue().toFloat();
    }

    // Apply global fade-in and slide-up for this item
    painter->setOpacity(anim_progress);
    painter->translate(0, 20.0f * (1.0f - anim_progress));

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
    QColor msgTextColor = isUser ? textUser_ : textColor_;
    QColor senderColor = isUser ? senderUser_ : senderAI_;
    if (isSystem) {
        senderColor = dimText_;
        msgTextColor = dimText_;
    }
    if (isHovered && !isSystem) {
        // Subtle brightness boost on hover
        if (isUser) {
            bubbleBg = QColor(79, 148, 255);   // Lighter blue
        } else {
            bubbleBg = hoverBg_;
        }
    }

    // ── Geometry ────────────────────────────────────────────────
    int viewWidth = option.rect.width();
    int maxBubbleWidth = static_cast<int>(viewWidth * kMaxBubbleRatio);

    QTextDocument doc;
    doc.setDefaultFont(messageFont());
    doc.setTextWidth(bubbleTextWidth(option));
    doc.setPlainText(text);
    int actualTextWidth = static_cast<int>(doc.idealWidth()) + 1;
    int textHeight = static_cast<int>(doc.size().height());

    // Bubble width: fit content but don't exceed max
    int bubbleContentWidth = std::min(actualTextWidth + kTimestampWidth,
                                       maxBubbleWidth - kBubblePadding * 2);
    int totalBubbleWidth = bubbleContentWidth + kBubblePadding * 2;

    // Position: user right-aligned, AI left-aligned
    int bubbleX = isUser ? (viewWidth - totalBubbleWidth - kBubbleMarginH)
                         : kBubbleMarginH;
    int bubbleY = option.rect.y() + kBubbleMarginV;
    int bubbleHeight = kSenderHeight + kBubblePadding + textHeight + kBubblePadding;

    QRect bubbleRect(bubbleX, bubbleY, totalBubbleWidth, bubbleHeight);

    // ── Draw bubble background with smooth rounded corners ──────
    QPainterPath path;
    path.addRoundedRect(bubbleRect, kBubbleRadius, kBubbleRadius);
    painter->fillPath(path, bubbleBg);

    // ── Subtle border for AI bubbles (premium glass effect) ─────
    if (!isUser && !isSystem) {
        painter->save();
        painter->setPen(QPen(QColor(63, 63, 70), 1.0));  // Zinc-700 border
        painter->drawPath(path);
        painter->restore();
    }

    // ── Draw sender name ────────────────────────────────────────
    int textStartX = bubbleRect.x() + kBubblePadding;
    int textAvailWidth = bubbleRect.width() - kBubblePadding * 2;

    QRect senderRect(textStartX, bubbleY + 6, textAvailWidth - kTimestampWidth, kSenderHeight);
    painter->setFont(senderFont());
    painter->setPen(senderColor);
    painter->drawText(senderRect, Qt::AlignLeft | Qt::AlignVCenter, sender);

    // ── Draw timestamp ──────────────────────────────────────────
    QRect timeRect(bubbleRect.right() - kBubblePadding - kTimestampWidth - kCopyBtnSize - 4,
                   bubbleY + 6, kTimestampWidth, kSenderHeight);
    painter->setFont(timestampFont());
    QColor timeColor = isUser ? QColor(191, 219, 254, 180) : dimText_;
    painter->setPen(timeColor);
    QString timeStr = timestamp.toString("hh:mm");
    painter->drawText(timeRect, Qt::AlignRight | Qt::AlignVCenter, timeStr);

    // ── Draw copy button ────────────────────────────────────────
    bool isCopied = index.data(CopiedStateRole).toBool();
    QRect copyBtn = getCopyButtonRect(bubbleRect);

    if (isCopied) {
        QFont copyFont = timestampFont();
        copyFont.setBold(true);
        painter->setFont(copyFont);
        painter->setPen(QColor(74, 222, 128));  // Green-400
        painter->drawText(copyBtn, Qt::AlignCenter, "\u2713");
    } else if (isHovered) {
        painter->setFont(timestampFont());
        painter->setPen(isUser ? QColor(255, 255, 255, 150) : dimText_);
        painter->drawText(copyBtn, Qt::AlignCenter, "\xF0\x9F\x93\x8B");
    }

    // ── Draw message text (with proper word-wrap) ───────────────
    QRect textRect(textStartX,
                   bubbleY + kSenderHeight + kBubblePadding - 2,
                   textAvailWidth, textHeight);

    painter->save();
    painter->translate(textRect.topLeft());
    painter->setPen(msgTextColor);

    QTextDocument renderDoc;
    renderDoc.setDefaultFont(messageFont());
    renderDoc.setTextWidth(textAvailWidth);
    renderDoc.setPlainText(text);

    QAbstractTextDocumentLayout::PaintContext ctx;
    ctx.palette.setColor(QPalette::Text, msgTextColor);
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
    view_->setMouseTracking(true);
    view_->setUniformItemSizes(false);
    view_->setSpacing(0);
    view_->setFocusPolicy(Qt::NoFocus);

    // Smooth scrolling
    view_->verticalScrollBar()->setSingleStep(20);

    // Layout
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(view_);

    setupStyle();

    connect(model_, &QAbstractListModel::rowsInserted, this, &ChatWidget::scrollToBottom);
}

void ChatWidget::setupStyle() {
    view_->setStyleSheet(
        "QListView {"
        "  background-color: #18181B;"    // Zinc-900
        "  border: none;"
        "  outline: none;"
        "}"
        // ── Premium thin scrollbar ──
        "QScrollBar:vertical {"
        "  background-color: transparent;"
        "  width: 6px;"
        "  margin: 4px 1px 4px 0px;"
        "}"
        "QScrollBar::handle:vertical {"
        "  background-color: #3F3F46;"    // Zinc-700
        "  min-height: 40px;"
        "  border-radius: 3px;"
        "}"
        "QScrollBar::handle:vertical:hover {"
        "  background-color: #8B5CF6;"    // Violet-500
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
    
    // Req 2: Trigger kinetic slide/fade for the freshly inserted message
    int row = model_->rowCount() - 1;
    delegate_->triggerNewMessageAnimation(row);
    scrollToBottom();
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
    QTimer::singleShot(10, this, [this]() {
        view_->scrollToBottom();
    });
}

} // namespace vision
