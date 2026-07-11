#pragma once

#include <QWidget>
#include <QIcon>

class QLabel;
class QToolButton;
class QTimer;

// Collapsible "thinking" panel shown above an assistant reply, for models
// that stream a separate reasoning trace (Ollama's message.thinking field —
// see docs.ollama.com/capabilities/thinking, distinct from the actual
// message.content). Starts collapsed, expands automatically the moment the
// first thinking token actually arrives (models that don't support
// thinking never trigger this at all — see ChatWidget::onChatThinkingDelta,
// which only creates one of these lazily), and auto-collapses again the
// moment real answer content starts arriving — mirroring Claude's own
// "Thought for Ns" UI. A manual click on the header disables that
// auto-collapse behavior for the rest of this turn, so the person's own
// choice always wins.
class ThinkingSectionWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ThinkingSectionWidget(QWidget *parent = nullptr);

    void appendThinkingText(const QString &text);

    // true while thinking tokens are actively streaming in; flip to false
    // once real answer content starts (or the turn ends) to freeze the
    // elapsed-time label and trigger the auto-collapse.
    void setThinking(bool isThinking);

private slots:
    void onHeaderClicked();

private:
    void updateHeaderText();
    void setExpanded(bool expanded);
    // Renders one animation frame of the "thinking" spinner as an icon
    // (a partial ring rotated to the given angle) — drawn on the fly rather
    // than shipping an asset, and read from the header button's own palette
    // so it tracks the light/dark theme automatically.
    QIcon spinnerIcon(int angleDegrees) const;

    QToolButton *m_headerButton = nullptr;
    QWidget *m_bodyContainer = nullptr;
    QLabel *m_bodyLabel = nullptr;

    QTimer *m_spinnerTimer = nullptr;
    int m_spinnerAngle = 0;

    QString m_buffer;
    bool m_isThinking = false;
    bool m_userToggled = false;

    qint64 m_startMs = 0;
    qint64 m_endMs = 0;
};
