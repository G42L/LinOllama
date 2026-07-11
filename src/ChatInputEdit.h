#pragma once

#include <QPlainTextEdit>
#include <QFrame>

// A minimal multi-line message box. Plain Enter and Shift+Enter both just
// insert a newline — that's QPlainTextEdit's normal behavior, nothing to
// override there. Ctrl+Enter submits instead. Grows with its content up to
// a cap (~9 lines) so a multi-line message is actually readable while
// typing, without letting a huge paste take over the window.
//
// This widget only reports the key event via submitRequested(); ChatWidget
// owns the actual "build the API call and send it" logic.
class ChatInputEdit : public QPlainTextEdit
{
    Q_OBJECT

public:
    explicit ChatInputEdit(QWidget *parent = nullptr);

signals:
    void submitRequested();

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void adjustHeight();

private:
    int m_minHeight = 40;
    int m_maxHeight = 220;
};
