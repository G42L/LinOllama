#pragma once

#include <QWidget>
#include <QLabel>
#include <QToolButton>

// One row in the conversation sidebar: a title label plus a "⋮" button that
// only becomes visible on hover (mirrors the pattern used by Claude/ChatGPT
// -style chat sidebars, rather than showing a menu button on every row all
// the time). This widget owns no deletion logic itself — it just emits
// deleteRequested(), which MainWindow wires up to a confirmation dialog. The
// sidebar's right-click context menu (also built in MainWindow) triggers the
// exact same slot, so there's one code path for "delete this chat" no matter
// how it was invoked.
class ConversationListItemWidget : public QWidget
{
    Q_OBJECT

public:
    ConversationListItemWidget(const QString &conversationId, const QString &title, QWidget *parent = nullptr);

    QString conversationId() const { return m_conversationId; }
    void setTitle(const QString &title);

signals:
    void deleteRequested(const QString &conversationId);
    void exportRequested(const QString &conversationId);

protected:
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    // Truncation width depends on the row's current width, so a resize
    // (e.g. dragging the sidebar splitter) needs to re-elide — see
    // updateElidedTitle().
    void resizeEvent(QResizeEvent *event) override;

private:
    // Elides m_fullTitle to fit the label's available width (reserving
    // constant space for the "⋮" menu button regardless of its current
    // hover-only visibility, so the truncation point doesn't visibly shift
    // when hovering) and sets a tooltip with the untruncated title, but only
    // when it's actually been truncated — no redundant tooltip otherwise.
    void updateElidedTitle();

    QString m_conversationId;
    QString m_fullTitle;
    QLabel *m_titleLabel = nullptr;
    QToolButton *m_menuButton = nullptr;
};
