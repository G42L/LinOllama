#pragma once

#include <QObject>
#include <QNetworkAccessManager>

// Best-effort "let the model look something up" search, since Ollama models
// have no network access of their own.
//
// Scope note (v1): backed by Wikipedia's public search API rather than a
// general web search engine. Keyless general-web search doesn't really
// exist for free — DuckDuckGo's HTML "lite" endpoint actively bot-blocks
// automated requests (confirmed: it returns a CAPTCHA challenge page on the
// very first request from a non-browser client), and DuckDuckGo's
// legitimate Instant-Answer JSON API returns empty for most ordinary
// queries (it's for infobox-style answers, not general search). Wikipedia's
// API is public, documented, reliable, and needs no key — good coverage for
// factual/encyclopedic questions, not for current events or arbitrary
// sites. A real general-purpose backend (e.g. Brave Search's API) would
// need a user-supplied key in Settings — straightforward to add later if
// broader coverage turns out to matter, same as this class's Whisper
// counterpart (VoiceRecorder) deferring real transcription to a later step.
class WebSearchClient : public QObject
{
    Q_OBJECT

public:
    explicit WebSearchClient(QObject *parent = nullptr);

    // Result always arrives via searchCompleted(), with an empty
    // resultsText if nothing could be found — callers don't need a
    // separate failure path for the common "no results" case.
    void search(const QString &query);

signals:
    void searchCompleted(const QString &query, const QString &resultsText);

private:
    QNetworkAccessManager m_manager;
};
