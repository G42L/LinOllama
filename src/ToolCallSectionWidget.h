#pragma once

#include <QWidget>
#include <QJsonObject>

class QLabel;
class QToolButton;

// Collapsible "tool call" panel shown above an assistant reply, for a turn
// where the model called one or more built-in tools before answering — the
// tool-calling counterpart to ThinkingSectionWidget, reusing its exact
// header/body styling (#thinkingHeader/#thinkingBody in Theme.cpp) since
// both are "collapsed-by-default meta info above the real answer." Unlike
// ThinkingSectionWidget, there's no live streaming or spinner: a call
// either hasn't resolved yet (constructed with an empty result, shows
// "Calling <tool>…") or has (setResult()), there's no in-between state to
// animate. Starts collapsed; a manual click toggles it same as thinking.
class ToolCallSectionWidget : public QWidget
{
    Q_OBJECT

public:
    // toolName/arguments are shown in the header/body immediately; the
    // result (if not yet known — e.g. web_search is still in flight) can be
    // filled in later via setResult().
    explicit ToolCallSectionWidget(const QString &toolName, const QJsonObject &arguments,
                                    QWidget *parent = nullptr);

    void setResult(const QString &resultText);

private slots:
    void onHeaderClicked();

private:
    void setExpanded(bool expanded);
    void updateHeaderText();
    void updateBodyText();
    // "key: value, key2: value2" — arguments are always simple string/
    // number/bool leaves for this app's built-in tools, so no need for
    // general recursive JSON formatting.
    static QString formatArguments(const QJsonObject &arguments);

    QToolButton *m_headerButton = nullptr;
    QWidget *m_bodyContainer = nullptr;
    QLabel *m_bodyLabel = nullptr;

    QString m_toolName;
    QJsonObject m_arguments;
    QString m_resultText;
    bool m_resolved = false;
    bool m_userToggled = false;
};
