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
    TypeRole,
    CopiedStateRole   // bool: true = show "Copied ✓" for 2s
};

// ═══════════════════ Model ═══════════════════════════════════════════

class ChatMessageModel : public QAbstractListModel {
    Q_OBJECT
public:
    explicit ChatMessageModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

    void addMessage(const QString& sender, const QString& text,
                    MessageType type = MessageType::AI);
    void clear();

    /// Stream a token to the last message
    void streamToken(const QString& token);

private:
    std::vector<ChatMessage> messages_;
    std::vector<bool> copied_states_;  // per-message "Copied ✓" flag
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

    /// Handle click on copy button (coordinate geometry check)
    bool editorEvent(QEvent* event, QAbstractItemModel* model,
                     const QStyleOptionViewItem& option,
                     const QModelIndex& index) override;

private:
    // Layout constants
    static constexpr int kBubblePadding = 14;
    static constexpr int kBubbleMarginH = 16;
    static constexpr int kBubbleMarginV = 4;
    static constexpr int kAccentBarWidth = 3;
    static constexpr int kSenderHeight = 20;
    static constexpr int kTimestampWidth = 50;
    static constexpr int kBubbleRadius = 10;
    static constexpr int kCopyBtnSize = 22;       // Copy button hit area
    static constexpr double kMaxBubbleRatio = 0.78;

    // Colors — Hacker Theme (Matrix Black + Neon Green)
    QColor bgAI_      = QColor(17, 17, 17);    // #111111
    QColor bgUser_    = QColor(26, 26, 26);    // #1A1A1A
    QColor bgSystem_  = QColor(10, 10, 10);    // #0A0A0A
    QColor accentAI_  = QColor(57, 255, 20);   // #39FF14 Neon Green
    QColor accentUser_= QColor(0, 200, 255);   // #00C8FF Cyberpunk Cyan
    QColor textColor_ = QColor(224, 224, 224); // #E0E0E0
    QColor dimText_   = QColor(85, 85, 85);    // #555555
    QColor hoverBg_   = QColor(26, 26, 26);    // #1A1A1A

    // Helpers
    QFont senderFont() const;
    QFont messageFont() const;
    QFont timestampFont() const;
    int bubbleTextWidth(const QStyleOptionViewItem& option) const;
    int calcTextHeight(const QString& text, int width) const;

    /// Shared geometry for copy button — used by BOTH paint() and editorEvent()
    QRect getCopyButtonRect(const QRect& bubbleRect) const;
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

    /// Stream a token to the last message (Req 2)
    void streamToken(const QString& token);

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
