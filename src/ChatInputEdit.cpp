#include "ChatInputEdit.h"

#include <QKeyEvent>
#include <QTextDocument>
#include <QTextBlock>
#include <QAbstractTextDocumentLayout>

ChatInputEdit::ChatInputEdit(QWidget *parent)
    : QPlainTextEdit(parent)
{
    setFrameShape(QFrame::NoFrame);
    setFixedHeight(m_minHeight);
    // Without this, Qt's default ScrollBarAsNeeded can toggle a scrollbar
    // on right as content grows past the current height, which narrows the
    // viewport, which changes wrapping, which changes the height again —
    // a resize feedback loop that can leave the box stuck instead of
    // settling. AutoHeightTextBrowser already avoids this the same way.
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    // documentSizeChanged fires once layout has actually been recomputed —
    // more reliable than textChanged, which can fire a frame before the new
    // size is available and make growth lag one keystroke behind.
    connect(document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged,
            this, &ChatInputEdit::adjustHeight);
}

void ChatInputEdit::keyPressEvent(QKeyEvent *event)
{
    const bool isEnter = (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter);
    if (isEnter && (event->modifiers() & Qt::ControlModifier)) {
        emit submitRequested();
        return; // swallow — don't also insert a newline
    }

    // Plain Enter and Shift+Enter both fall through to QPlainTextEdit's
    // default handling, which is simply "insert a newline". That's the
    // whole point of this widget existing instead of the old single-line
    // QLineEdit, which had no concept of a newline at all.
    QPlainTextEdit::keyPressEvent(event);
}

void ChatInputEdit::resizeEvent(QResizeEvent *event)
{
    QPlainTextEdit::resizeEvent(event);
    // A width change affects wrapping, which affects how many lines the
    // current text needs — recompute rather than waiting for the next
    // keystroke to notice.
    adjustHeight();
}

void ChatInputEdit::adjustHeight()
{
    // document()->size().height() is unreliable here: QPlainTextEdit's
    // QPlainTextDocumentLayout lays out each block's own line-wrapped
    // bounding rect correctly, but never computes the aggregate document
    // size/stacking outside of actual on-screen painting — so size() can
    // stay stuck at a stale near-zero value no matter how much text there
    // is. Summing each block's own (correctly-computed) bounding rect
    // height sidesteps that and reflects wrapped content accurately.
    QAbstractTextDocumentLayout *docLayout = document()->documentLayout();
    qreal contentHeight = 0;
    for (QTextBlock block = document()->begin(); block.isValid(); block = block.next())
        contentHeight += docLayout->blockBoundingRect(block).height();

    const int newHeight = qBound(m_minHeight, static_cast<int>(contentHeight) + 12, m_maxHeight);
    if (newHeight != height())
        setFixedHeight(newHeight);
}
