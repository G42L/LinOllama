#include "ThinkingSectionWidget.h"
#include "EmojiRenderer.h"

#include <QVBoxLayout>
#include <QLabel>
#include <QToolButton>
#include <QDateTime>
#include <QTimer>
#include <QPainter>
#include <QPixmap>
#include <QFontInfo>

ThinkingSectionWidget::ThinkingSectionWidget(QWidget *parent)
    : QWidget(parent)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 6);
    outer->setSpacing(2);

    m_headerButton = new QToolButton;
    m_headerButton->setObjectName("thinkingHeader");
    m_headerButton->setCursor(Qt::PointingHandCursor);
    m_headerButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_headerButton->setIconSize(QSize(13, 13));
    m_headerButton->setArrowType(Qt::RightArrow);
    m_headerButton->setAutoRaise(true);
    connect(m_headerButton, &QToolButton::clicked, this, &ThinkingSectionWidget::onHeaderClicked);
    outer->addWidget(m_headerButton);

    // Ticks only while m_isThinking is true (started/stopped in
    // setThinking()) — each tick advances the spinner icon one frame.
    m_spinnerTimer = new QTimer(this);
    m_spinnerTimer->setInterval(80);
    connect(m_spinnerTimer, &QTimer::timeout, this, [this]() {
        m_spinnerAngle = (m_spinnerAngle + 30) % 360;
        m_headerButton->setIcon(spinnerIcon(m_spinnerAngle));
    });

    m_bodyContainer = new QWidget;
    auto *bodyLayout = new QVBoxLayout(m_bodyContainer);
    bodyLayout->setContentsMargins(18, 2, 4, 4);

    m_bodyLabel = new QLabel;
    m_bodyLabel->setObjectName("thinkingBody");
    m_bodyLabel->setWordWrap(true);
    m_bodyLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // Reasoning text is shown literally, never as Markdown — but still runs
    // through EmojiRenderer, hence RichText rather than the default
    // auto-detected format (see appendThinkingText()).
    m_bodyLabel->setTextFormat(Qt::RichText);
    bodyLayout->addWidget(m_bodyLabel);

    outer->addWidget(m_bodyContainer);
    m_bodyContainer->setVisible(false); // nothing to show until the first token arrives

    updateHeaderText();
}

void ThinkingSectionWidget::appendThinkingText(const QString &text)
{
    if (m_buffer.isEmpty())
        m_startMs = QDateTime::currentMSecsSinceEpoch();

    m_buffer += text;
    m_bodyLabel->ensurePolished();
    const int pixelSize = QFontInfo(m_bodyLabel->font()).pixelSize();
    m_bodyLabel->setText(QStringLiteral("<div style=\"white-space:pre-wrap;\">%1</div>")
                              .arg(EmojiRenderer::escapedPlainTextWithImages(m_buffer, pixelSize)));

    // Auto-expand as thinking content starts arriving, unless the person
    // has already touched the toggle themselves this turn.
    if (!m_userToggled)
        setExpanded(true);
}

void ThinkingSectionWidget::setThinking(bool isThinking)
{
    if (m_isThinking == isThinking)
        return;
    m_isThinking = isThinking;

    if (isThinking) {
        m_spinnerAngle = 0;
        m_headerButton->setIcon(spinnerIcon(m_spinnerAngle));
        m_spinnerTimer->start();
    } else {
        m_spinnerTimer->stop();
        m_headerButton->setIcon(QIcon()); // done thinking — the elapsed-time text says it all
        m_endMs = QDateTime::currentMSecsSinceEpoch();
    }

    updateHeaderText();

    // The moment real answer content starts arriving, auto-collapse — same
    // behavior as Claude's own UI — unless the person already toggled it.
    if (!isThinking && !m_userToggled && !m_buffer.isEmpty())
        setExpanded(false);
}

void ThinkingSectionWidget::onHeaderClicked()
{
    m_userToggled = true;
    setExpanded(!m_bodyContainer->isVisible());
}

void ThinkingSectionWidget::setExpanded(bool expanded)
{
    m_bodyContainer->setVisible(expanded);
    m_headerButton->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
}

void ThinkingSectionWidget::updateHeaderText()
{
    QString label;
    if (m_isThinking || m_startMs == 0) {
        label = "Thinking…";
    } else {
        const double seconds = (m_endMs - m_startMs) / 1000.0;
        label = seconds < 1.0
            ? "Thought briefly"
            : QString("Thought for %1s").arg(seconds, 0, 'f', seconds < 10 ? 1 : 0);
    }
    m_headerButton->setText(label);
}

QIcon ThinkingSectionWidget::spinnerIcon(int angleDegrees) const
{
    QPixmap pixmap(13, 13);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    // Reads the button's own (QSS-applied) foreground color rather than a
    // hardcoded hex, so this stays correct in both light and dark themes.
    QPen pen(m_headerButton->palette().color(QPalette::WindowText));
    pen.setWidthF(1.6);
    pen.setCapStyle(Qt::RoundCap);
    painter.setPen(pen);

    // A 3/4 ring rather than a full circle is what reads as "spinning"
    // once it's rotating — a full circle would look static every frame.
    const QRectF ring(1.5, 1.5, 10, 10);
    painter.drawArc(ring, angleDegrees * 16, 270 * 16);

    return QIcon(pixmap);
}
