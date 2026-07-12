#include "ConversationListItemWidget.h"

#include <QHBoxLayout>
#include <QMenu>
#include <QWidgetAction>
#include <QFontMetrics>
#include <QResizeEvent>

ConversationListItemWidget::ConversationListItemWidget(const QString &conversationId,
                                                         const QString &title,
                                                         QWidget *parent)
    : QWidget(parent)
    , m_conversationId(conversationId)
    , m_fullTitle(title)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 4, 4, 4);
    layout->setSpacing(4);

    // Text is set by updateElidedTitle() once the row has a real width
    // (see resizeEvent) — starts with the full title so there's something
    // to show before the first layout pass.
    m_titleLabel = new QLabel(title);
    m_titleLabel->setObjectName("conversationTitle");
    // A QLabel's default minimumSizeHint() is based on its full (un-elided)
    // text, which would otherwise stop the layout from ever actually
    // shrinking it below that — defeating updateElidedTitle() even once
    // this widget's own width is correct. 0 lets the layout shrink it
    // freely; the label's own displayed text is what actually controls how
    // much room it visually needs.
    m_titleLabel->setMinimumWidth(0);
    layout->addWidget(m_titleLabel, /*stretch=*/1);

    m_menuButton = new QToolButton;
    m_menuButton->setObjectName("conversationMenuButton");
    m_menuButton->setText(QString::fromUtf8("\xE2\x8B\xAE")); // "⋮" vertical ellipsis
    m_menuButton->setAutoRaise(true);
    m_menuButton->setCursor(Qt::PointingHandCursor);
    m_menuButton->setVisible(false); // only shown on hover — see enterEvent/leaveEvent
    layout->addWidget(m_menuButton);

    connect(m_menuButton, &QToolButton::clicked, this, [this]() {
        auto *menu = new QMenu(this);
        // See MainWindow::buildDeleteMenu()'s own comment — same rounded-
        // corner background-halo fix, needed at every QMenu construction
        // site individually since it's a per-window attribute.
        menu->setAttribute(Qt::WA_TranslucentBackground);

        QAction *exportAction = menu->addAction("Export conversation…");
        connect(exportAction, &QAction::triggered, this, [this]() {
            emit exportRequested(m_conversationId);
        });

        // A styled QLabel rather than a plain QAction so the delete entry
        // can be given red "danger" text — see Theme.cpp's #deleteMenuItem rule.
        auto *deleteLabel = new QLabel("  Delete chat  ");
        deleteLabel->setObjectName("deleteMenuItem");

        auto *deleteAction = new QWidgetAction(menu);
        deleteAction->setDefaultWidget(deleteLabel);
        menu->addAction(deleteAction);

        connect(deleteAction, &QAction::triggered, this, [this]() {
            emit deleteRequested(m_conversationId);
        });

        menu->exec(m_menuButton->mapToGlobal(QPoint(0, m_menuButton->height())));
        menu->deleteLater();
    });
}

void ConversationListItemWidget::setTitle(const QString &title)
{
    m_fullTitle = title;
    updateElidedTitle();
}

void ConversationListItemWidget::enterEvent(QEnterEvent *event)
{
    Q_UNUSED(event);
    m_menuButton->setVisible(true);
}

void ConversationListItemWidget::leaveEvent(QEvent *event)
{
    Q_UNUSED(event);
    m_menuButton->setVisible(false);
}

void ConversationListItemWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateElidedTitle();
}

void ConversationListItemWidget::updateElidedTitle()
{
    // sizeHint() (not the current, hover-dependent width) so the reserved
    // space — and therefore the truncation point — stays constant whether
    // or not the button happens to be visible right now.
    const int reserved = m_menuButton->sizeHint().width() + 18; // + layout margins/spacing
    const int available = qMax(20, width() - reserved);

    const QFontMetrics fm(m_titleLabel->font());
    const QString elided = fm.elidedText(m_fullTitle, Qt::ElideRight, available);
    m_titleLabel->setText(elided);
    m_titleLabel->setToolTip(elided != m_fullTitle ? m_fullTitle : QString());
}
