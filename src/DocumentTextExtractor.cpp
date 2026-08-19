#include "DocumentTextExtractor.h"

#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QXmlStreamReader>

namespace {

QString extractPlainText(const QString &filePath, QString *errorOut)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorOut)
            *errorOut = "Couldn't open \"" + filePath + "\".";
        return QString();
    }

    const QByteArray bytes = file.readAll();
    if (bytes.contains('\0')) {
        if (errorOut)
            *errorOut = "\"" + filePath + "\" looks like a binary file, not plain text.";
        return QString();
    }

    return QString::fromUtf8(bytes);
}

// Runs `program args...` and returns its stdout, or an empty string with
// *errorOut set on a missing binary, non-zero exit, or empty output —
// shared by the PDF (pdftotext) and DOCX (unzip) paths below, both of
// which are a single "run this CLI tool, capture stdout" call.
QString runToolCapturingStdout(const QString &program, const QStringList &args,
                                const QString &missingBinaryHint, QString *errorOut)
{
    if (QStandardPaths::findExecutable(program).isEmpty()) {
        if (errorOut)
            *errorOut = missingBinaryHint;
        return QString();
    }

    QProcess process;
    process.start(program, args);
    // Extraction runs on a worker thread (see RagIngestionController), so
    // blocking here doesn't stall the UI — a generous timeout still guards
    // against a genuinely hung/corrupt-input invocation never returning.
    if (!process.waitForFinished(60000)) {
        if (errorOut)
            *errorOut = QString("%1 timed out.").arg(program);
        process.kill();
        return QString();
    }

    const QByteArray output = process.readAllStandardOutput();
    if (process.exitCode() != 0 || output.isEmpty()) {
        if (errorOut) {
            const QString stderrText = QString::fromUtf8(process.readAllStandardError()).trimmed();
            *errorOut = stderrText.isEmpty()
                ? QString("%1 produced no output.").arg(program)
                : QString("%1: %2").arg(program, stderrText);
        }
        return QString();
    }

    return QString::fromUtf8(output);
}

QString extractPdfText(const QString &filePath, QString *errorOut)
{
    return runToolCapturingStdout(
        "pdftotext", {"-layout", filePath, "-"},
        "PDF support requires poppler-utils: sudo apt install poppler-utils", errorOut);
}

// word/document.xml uses <w:t> elements for actual visible text and <w:p>
// for paragraph boundaries — walking it with QXmlStreamReader (already part
// of Qt6::Core, no new dependency) is precise and far more robust than a
// regex tag-stripper against nested/self-closing XML.
QString stripDocxXml(const QByteArray &xml)
{
    QString text;
    QXmlStreamReader reader(xml);
    bool insideText = false;
    while (!reader.atEnd()) {
        const QXmlStreamReader::TokenType token = reader.readNext();
        if (token == QXmlStreamReader::StartElement) {
            if (reader.name() == QLatin1String("t"))
                insideText = true;
        } else if (token == QXmlStreamReader::EndElement) {
            if (reader.name() == QLatin1String("t"))
                insideText = false;
            else if (reader.name() == QLatin1String("p"))
                text += "\n";
        } else if (token == QXmlStreamReader::Characters && insideText) {
            text += reader.text();
        }
    }
    return text;
}

QString extractDocxText(const QString &filePath, QString *errorOut)
{
    QString unzipError;
    const QString stdOut = runToolCapturingStdout(
        "unzip", {"-p", filePath, "word/document.xml"},
        "DOCX support requires unzip: sudo apt install unzip", &unzipError);
    if (stdOut.isEmpty()) {
        if (errorOut)
            *errorOut = unzipError;
        return QString();
    }

    const QString text = stripDocxXml(stdOut.toUtf8()).trimmed();
    if (text.isEmpty() && errorOut)
        *errorOut = "\"" + filePath + "\" doesn't look like a valid .docx (no readable text found).";
    return text;
}

} // namespace

namespace DocumentTextExtractor {

QString extractText(const QString &filePath, QString *errorOut)
{
    if (errorOut)
        errorOut->clear();

    const QString suffix = QFileInfo(filePath).suffix().toLower();
    if (suffix == "txt" || suffix == "md")
        return extractPlainText(filePath, errorOut);
    if (suffix == "pdf")
        return extractPdfText(filePath, errorOut);
    if (suffix == "docx")
        return extractDocxText(filePath, errorOut);

    if (errorOut)
        *errorOut = "Unsupported file type \"." + suffix + "\" — supported: PDF, DOCX, TXT, MD.";
    return QString();
}

QString fileDialogFilter()
{
    return "Documents (*.pdf *.docx *.txt *.md);;All files (*)";
}

} // namespace DocumentTextExtractor
