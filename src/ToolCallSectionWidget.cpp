#include "ToolCallSectionWidget.h"

#include <QVBoxLayout>
#include <QLabel>
#include <QToolButton>
#include <QJsonValue>

ToolCallSectionWidget::ToolCallSectionWidget(const QString &toolName, const QJsonObject &arguments,
                                               QWidget *parent)
    : QWidget(parent)
    , m_toolName(toolName)
    , m_arguments(arguments)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 6);
    outer->setSpacing(2);

    // Same object names as ThinkingSectionWidget's header/body — see this
    // class's own header comment for why: visually it's the same "collapsed
    // meta info above the answer" pattern, so it should look identical
    // rather than needing its own duplicate Theme.cpp rules.
    m_headerButton = new QToolButton;
    m_headerButton->setObjectName("thinkingHeader");
    m_headerButton->setCursor(Qt::PointingHandCursor);
    m_headerButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_headerButton->setArrowType(Qt::RightArrow);
    m_headerButton->setAutoRaise(true);
    connect(m_headerButton, &QToolButton::clicked, this, &ToolCallSectionWidget::onHeaderClicked);
    outer->addWidget(m_headerButton);

    m_bodyContainer = new QWidget;
    auto *bodyLayout = new QVBoxLayout(m_bodyContainer);
    bodyLayout->setContentsMargins(18, 2, 4, 4);

    m_bodyLabel = new QLabel;
    m_bodyLabel->setObjectName("thinkingBody");
    m_bodyLabel->setWordWrap(true);
    m_bodyLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    bodyLayout->addWidget(m_bodyLabel);

    outer->addWidget(m_bodyContainer);
    m_bodyContainer->setVisible(false);

    updateHeaderText();
    updateBodyText();
}

void ToolCallSectionWidget::setResult(const QString &resultText)
{
    m_resultText = resultText;
    m_resolved = true;
    updateHeaderText();
    updateBodyText();
}

void ToolCallSectionWidget::onHeaderClicked()
{
    m_userToggled = true;
    setExpanded(!m_bodyContainer->isVisible());
}

void ToolCallSectionWidget::setExpanded(bool expanded)
{
    m_bodyContainer->setVisible(expanded);
    m_headerButton->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
}

void ToolCallSectionWidget::updateHeaderText()
{
    m_headerButton->setText(m_resolved
        ? QString("Called %1").arg(m_toolName)
        : QString("Calling %1…").arg(m_toolName));
}

void ToolCallSectionWidget::updateBodyText()
{
    QString text = formatArguments(m_arguments);
    if (!text.isEmpty())
        text += "\n\n";
    text += m_resolved ? m_resultText : QStringLiteral("Running…");
    m_bodyLabel->setText(text);
}

QString ToolCallSectionWidget::formatArguments(const QJsonObject &arguments)
{
    // This app's built-in tools only ever take simple scalar arguments
    // (a search query string, an expression string) — no need for general
    // recursive JSON-value formatting.
    QStringList parts;
    for (auto it = arguments.constBegin(); it != arguments.constEnd(); ++it) {
        const QJsonValue &v = it.value();
        QString valueText;
        if (v.isString())
            valueText = v.toString();
        else if (v.isBool())
            valueText = v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
        else if (v.isDouble())
            valueText = QString::number(v.toDouble());
        else
            valueText = QStringLiteral("(complex value)");
        parts << QString("%1: %2").arg(it.key(), valueText);
    }
    return parts.join(", ");
}
