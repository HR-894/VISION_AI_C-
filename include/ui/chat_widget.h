#pragma once
/**
 * @file chat_widget.h
 * @brief Modern conversational chat UI — QListView + Custom Delegate
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
#include <QVariantAnimation>
#include <QEasingCurve>

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

    /// Trigger animation for a newly added message (Req 2)
    void triggerNewMessageAnimation(int row);

    /// Handle click on copy button (coordinate geometry check)
    bool editorEvent(QEvent* event, QAbstractItemModel* model,
                     const QStyleOptionViewItem& option,
                     const QModelIndex& index) override;

private:
    QVariantAnimation* add_anim_{nullptr};
    int animating_row_{-1};

    // Layout constants — Premium Modern Chat
    static constexpr int kBubblePadding = 16;
    static constexpr int kBubbleMarginH = 20;
    static constexpr int kBubbleMarginV = 3;
    static constexpr int kAccentBarWidth = 0;       // No accent bar — clean bubbles
    static constexpr int kSenderHeight = 22;
    static constexpr int kTimestampWidth = 50;
    static constexpr int kBubbleRadius = 18;
    static constexpr int kCopyBtnSize = 22;
    static constexpr double kMaxBubbleRatio = 0.82;

    // Colors — Premium Deep Dark + Vibrant Blue Accent
    QColor bgAI_      = QColor(39, 39, 42);     // #27272A  Zinc-800
    QColor bgUser_    = QColor(59, 130, 246);    // #3B82F6  Vivid Blue
    QColor bgSystem_  = QColor(24, 24, 27);      // #18181B  Zinc-900
    QColor accentAI_  = QColor(139, 92, 246);    // #8B5CF6  Violet
    QColor accentUser_= QColor(59, 130, 246);    // #3B82F6  Blue
    QColor textColor_ = QColor(228, 228, 231);   // #E4E4E7  Zinc-200
    QColor textUser_  = QColor(255, 255, 255);   // #FFFFFF  White on blue
    QColor dimText_   = QColor(113, 113, 122);   // #71717A  Zinc-500
    QColor hoverBg_   = QColor(50, 50, 56);      // #323238  Hover glow
    QColor senderAI_  = QColor(167, 139, 250);   // #A78BFA  Violet-400
    QColor senderUser_= QColor(191, 219, 254);   // #BFDBFE  Blue-200

    // Helpers
    QFont senderFont() const;
    QFont messageFont() const;
    QFont timestampFont() const;
    int bubbleTextWidth(const QStyleOptionViewItem& option) const;
    int calcTextHeight(const QString& text, int width) const;

    /// Shared geometry for copy button
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
