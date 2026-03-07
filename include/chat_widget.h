#pragma once
/**
 * @file chat_widget.h
 * @brief Discord-tier chat UI — QListView + Custom Delegate
 *
 * Replaces QTextEdit with a proper MVC chat widget:
 * - ChatMessage: Data struct
 * - ChatMessageModel: QAbstractListModel
 * - ChatMessageDelegate: QPainter-based bubble renderer
 * - ChatWidget: Wrapper widget with public API
 */

#include <QWidget>
#include <QListView>
#include <QAbstractListModel>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QTimer>
#include <QString>
#include <QDateTime>
#include <vector>

namespace vision {

// ═══════════════════ Data ═══════════════════════════════════════════

enum class MessageType { User, AI, System };

struct ChatMessage {
    QString sender;
    QString text;
    QDateTime timestamp;
    MessageType type;
};

// Custom data roles for the model
enum ChatRoles {
    SenderRole = Qt::UserRole + 1,
    TextRole,
    TimestampRole,
    TypeRole
};

// ═══════════════════ Model ═══════════════════════════════════════════

class ChatMessageModel : public QAbstractListModel {
    Q_OBJECT
public:
    explicit ChatMessageModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;

    void addMessage(const QString& sender, const QString& text,
                    MessageType type = MessageType::AI);
    void clear();

private:
    std::vector<ChatMessage> messages_;
};

// ═══════════════════ Delegate ════════════════════════════════════════

class ChatMessageDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit ChatMessageDelegate(QObject* parent = nullptr);

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;

    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;

private:
    // Layout constants
    static constexpr int kBubblePadding = 14;
    static constexpr int kBubbleMarginH = 16;     // Horizontal margin from edges
    static constexpr int kBubbleMarginV = 4;       // Vertical gap between bubbles
    static constexpr int kAccentBarWidth = 3;      // Left color bar for AI messages
    static constexpr int kSenderHeight = 20;       // Sender name line height
    static constexpr int kTimestampWidth = 50;     // Space reserved for timestamp
    static constexpr int kBubbleRadius = 10;       // Corner radius
    static constexpr double kMaxBubbleRatio = 0.78; // Max bubble width = 78% of view

    // Colors
    QColor bgAI_      = QColor(43, 45, 49);    // #2b2d31
    QColor bgUser_    = QColor(56, 58, 64);    // #383a40
    QColor bgSystem_  = QColor(30, 31, 34);    // #1e1f22
    QColor accentAI_  = QColor(88, 101, 242);  // #5865F2 Discord Blurple
    QColor accentUser_= QColor(87, 242, 135);  // #57F287 Green
    QColor textColor_ = QColor(219, 222, 225); // #dbdee1
    QColor dimText_   = QColor(148, 155, 164); // #949ba4
    QColor hoverBg_   = QColor(63, 63, 70);    // #3f3f46

    // Helpers
    QFont senderFont() const;
    QFont messageFont() const;
    QFont timestampFont() const;
    int bubbleTextWidth(const QStyleOptionViewItem& option) const;
    int calcTextHeight(const QString& text, int width) const;
};

// ═══════════════════ Widget ═════════════════════════════════════════

class ChatWidget : public QWidget {
    Q_OBJECT
public:
    explicit ChatWidget(QWidget* parent = nullptr);

    /// Add a message to the chat (thread-safe when called from signals)
    void addMessage(const QString& sender, const QString& text,
                    MessageType type = MessageType::AI);

    /// Clear all messages
    void clear();

    /// Get message count
    int messageCount() const;

private:
    QListView* view_;
    ChatMessageModel* model_;
    ChatMessageDelegate* delegate_;

    void scrollToBottom();
    void setupStyle();
};

} // namespace vision
