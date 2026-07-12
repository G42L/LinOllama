#pragma once

#include <QObject>
#include <QNetworkAccessManager>

// Search backend for the "Search Stack Overflow" tool — backed by the
// public Stack Exchange API (api.stackexchange.com), which unlike a general
// web search engine is free, keyless, and explicitly meant for exactly this
// kind of programmatic use (same reasoning as WebSearchClient's own
// Wikipedia choice, see its header comment). Scoped to Stack Overflow only,
// not the wider Stack Exchange network (Super User, Server Fault, etc.) —
// easy to widen later (site= is just a query parameter) if that turns out
// to matter.
class StackOverflowSearchClient : public QObject
{
    Q_OBJECT

public:
    explicit StackOverflowSearchClient(QObject *parent = nullptr);

    // Result always arrives via searchCompleted(), with an empty
    // resultsText if nothing could be found — callers don't need a
    // separate failure path for the common "no results" case.
    void search(const QString &query);

signals:
    void searchCompleted(const QString &query, const QString &resultsText);

private:
    QNetworkAccessManager m_manager;
};
