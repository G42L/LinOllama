#include "CodeHighlighter.h"
#include "Theme.h"

#include <QRegularExpression>
#include <QSet>
#include <QHash>

namespace {

enum class Kind { Generic, Html, PlainOnly };

struct LangRules
{
    QSet<QString> keywords;
    bool caseInsensitiveKeywords = false;
    bool hashComment = false;      // # ... to end of line
    bool slashSlashComment = false; // // ... to end of line
    bool dashDashComment = false;  // -- ... to end of line (SQL)
    bool slashStarComment = false; // /* ... */
    bool singleQuoteString = true;
    bool doubleQuoteString = true;
    bool backtickString = false;
    bool tripleQuoteString = false; // Python-style '''...'''/"""..."""
};

QString span(const QString &colorHex, const QString &escapedText)
{
    if (escapedText.isEmpty())
        return escapedText;
    return QStringLiteral("<span style=\"color:%1;\">%2</span>").arg(colorHex, escapedText);
}

// Canonicalizes a fenced code block's language tag to one of the keys
// switchLangRules()/Kind selection below understands — covers the common
// aliases people actually type (js/ts, sh/zsh/shell, py, yml, c++/cc/cxx,
// etc.), not just the canonical name.
QString canonicalLanguage(const QString &language)
{
    const QString l = language.trimmed().toLower();
    static const QHash<QString, QString> aliases = {
        {"py", "python"},
        {"sh", "bash"}, {"zsh", "bash"}, {"shell", "bash"}, {"bash", "bash"},
        {"js", "javascript"}, {"javascript", "javascript"}, {"jsx", "javascript"},
        {"ts", "typescript"}, {"typescript", "typescript"}, {"tsx", "typescript"},
        {"c++", "cpp"}, {"cc", "cpp"}, {"cxx", "cpp"}, {"cpp", "cpp"}, {"h", "cpp"}, {"hpp", "cpp"},
        {"c", "c"},
        {"cs", "csharp"}, {"c#", "csharp"}, {"csharp", "csharp"},
        {"golang", "go"}, {"go", "go"},
        {"rs", "rust"}, {"rust", "rust"},
        {"rb", "ruby"}, {"ruby", "ruby"},
        {"php", "php"},
        {"java", "java"},
        {"sql", "sql"},
        {"json", "json"},
        {"yml", "yaml"}, {"yaml", "yaml"},
        {"html", "html"}, {"htm", "html"}, {"xml", "html"}, {"svg", "html"},
        {"css", "css"},
    };
    return aliases.value(l, l);
}

const LangRules &rulesFor(const QString &canonical, bool *found)
{
    static const QHash<QString, LangRules> table = [] {
        QHash<QString, LangRules> t;

        LangRules python;
        python.hashComment = true;
        python.tripleQuoteString = true;
        python.keywords = {
            "def", "class", "return", "if", "elif", "else", "for", "while", "import", "from",
            "as", "try", "except", "finally", "with", "pass", "break", "continue", "lambda",
            "yield", "in", "is", "not", "and", "or", "None", "True", "False", "self", "cls",
            "async", "await", "global", "nonlocal", "raise", "del", "assert",
        };
        t["python"] = python;

        LangRules bash;
        bash.hashComment = true;
        bash.keywords = {
            "if", "then", "else", "elif", "fi", "for", "while", "do", "done", "case", "esac",
            "function", "return", "in", "local", "export", "echo", "exit", "break", "continue",
            "select", "until", "time", "readonly", "shift", "trap", "set", "unset",
        };
        t["bash"] = bash;

        LangRules js;
        js.slashSlashComment = true;
        js.slashStarComment = true;
        js.backtickString = true;
        js.keywords = {
            "function", "return", "if", "else", "for", "while", "do", "switch", "case", "default",
            "break", "continue", "var", "let", "const", "class", "extends", "new", "this",
            "typeof", "instanceof", "in", "of", "try", "catch", "finally", "throw", "async",
            "await", "yield", "import", "export", "from", "as", "null", "undefined", "true",
            "false", "super", "static", "get", "set", "void", "delete",
        };
        t["javascript"] = js;

        LangRules ts = js;
        ts.keywords += QSet<QString>{
            "interface", "type", "enum", "implements", "namespace", "readonly", "public",
            "private", "protected", "abstract", "declare", "is", "keyof", "infer",
        };
        t["typescript"] = ts;

        LangRules java;
        java.slashSlashComment = true;
        java.slashStarComment = true;
        java.keywords = {
            "public", "private", "protected", "class", "interface", "extends", "implements",
            "return", "if", "else", "for", "while", "do", "switch", "case", "default", "break",
            "continue", "new", "this", "super", "static", "final", "void", "int", "long",
            "double", "float", "boolean", "char", "byte", "short", "String", "try", "catch",
            "finally", "throw", "throws", "import", "package", "null", "true", "false", "enum",
            "abstract", "synchronized", "instanceof",
        };
        t["java"] = java;

        LangRules c;
        c.slashSlashComment = true;
        c.slashStarComment = true;
        c.keywords = {
            "int", "long", "double", "float", "char", "void", "if", "else", "for", "while", "do",
            "switch", "case", "default", "break", "continue", "return", "struct", "static",
            "const", "unsigned", "signed", "sizeof", "typedef", "enum", "union", "extern",
            "include", "define", "true", "false", "NULL",
        };
        t["c"] = c;

        LangRules cpp = c;
        cpp.keywords += QSet<QString>{
            "class", "public", "private", "protected", "virtual", "override", "namespace",
            "using", "template", "typename", "new", "delete", "this", "nullptr", "auto",
            "bool", "try", "catch", "throw", "explicit", "friend", "operator", "constexpr",
            "noexcept", "inline",
        };
        t["cpp"] = cpp;

        LangRules csharp = cpp;
        csharp.keywords += QSet<QString>{
            "namespace", "using", "public", "private", "protected", "internal", "class",
            "interface", "struct", "enum", "readonly", "static", "override", "virtual",
            "async", "await", "var", "string", "object", "null", "true", "false", "get", "set",
        };
        t["csharp"] = csharp;

        LangRules go;
        go.slashSlashComment = true;
        go.slashStarComment = true;
        go.backtickString = true;
        go.keywords = {
            "func", "package", "import", "return", "if", "else", "for", "range", "switch",
            "case", "default", "break", "continue", "var", "const", "type", "struct",
            "interface", "map", "chan", "go", "defer", "select", "nil", "true", "false",
        };
        t["go"] = go;

        LangRules rust;
        rust.slashSlashComment = true;
        rust.slashStarComment = true;
        rust.keywords = {
            "fn", "let", "mut", "return", "if", "else", "for", "while", "loop", "match",
            "struct", "enum", "impl", "trait", "pub", "use", "mod", "self", "Self", "true",
            "false", "None", "Some", "Ok", "Err", "async", "await", "move", "ref", "static",
            "const", "unsafe", "where", "as", "in",
        };
        t["rust"] = rust;

        LangRules php;
        php.hashComment = true;
        php.slashSlashComment = true;
        php.slashStarComment = true;
        php.keywords = {
            "function", "return", "if", "else", "elseif", "endif", "for", "foreach", "while",
            "do", "switch", "case", "default", "break", "continue", "class", "extends",
            "implements", "public", "private", "protected", "static", "const", "new", "this",
            "echo", "print", "use", "namespace", "try", "catch", "finally", "throw", "null",
            "true", "false", "array", "as",
        };
        t["php"] = php;

        LangRules ruby;
        ruby.hashComment = true;
        ruby.keywords = {
            "def", "end", "return", "if", "elsif", "else", "unless", "for", "while", "until",
            "class", "module", "do", "break", "next", "case", "when", "then", "begin", "rescue",
            "ensure", "raise", "nil", "true", "false", "self", "require", "include", "yield",
            "attr_accessor", "attr_reader", "attr_writer", "and", "or", "not", "in",
        };
        t["ruby"] = ruby;

        LangRules sql;
        sql.dashDashComment = true;
        sql.slashStarComment = true;
        sql.caseInsensitiveKeywords = true;
        sql.keywords = {
            "SELECT", "FROM", "WHERE", "INSERT", "INTO", "VALUES", "UPDATE", "SET", "DELETE",
            "CREATE", "TABLE", "ALTER", "DROP", "JOIN", "INNER", "LEFT", "RIGHT", "OUTER", "ON",
            "GROUP", "BY", "ORDER", "HAVING", "AS", "AND", "OR", "NOT", "NULL", "IS", "IN",
            "LIKE", "LIMIT", "DISTINCT", "UNION", "EXISTS", "PRIMARY", "KEY", "FOREIGN",
            "REFERENCES", "DEFAULT", "INDEX", "VALUES", "INTO", "CASE", "WHEN", "THEN", "END",
        };
        t["sql"] = sql;

        LangRules json;
        json.keywords = {"true", "false", "null"};
        t["json"] = json;

        LangRules yaml;
        yaml.hashComment = true;
        yaml.keywords = {"true", "false", "null", "yes", "no", "on", "off"};
        t["yaml"] = yaml;

        LangRules css;
        css.slashStarComment = true;
        t["css"] = css;

        return t;
    }();

    const auto it = table.constFind(canonical);
    *found = (it != table.constEnd());
    static const LangRules empty;
    return *found ? it.value() : empty;
}

Kind kindFor(const QString &canonical)
{
    if (canonical == QLatin1String("html"))
        return Kind::Html;
    bool found = false;
    rulesFor(canonical, &found);
    return found ? Kind::Generic : Kind::PlainOnly;
}

bool isWordStart(const QChar &c) { return c.isLetter() || c == QLatin1Char('_'); }
bool isWordChar(const QChar &c) { return c.isLetterOrNumber() || c == QLatin1Char('_'); }

QString highlightGeneric(const QString &code, const LangRules &rules, bool dark)
{
    const QString kwColor = Theme::colorToken(QStringLiteral("codeKeyword"), dark);
    const QString strColor = Theme::colorToken(QStringLiteral("codeString"), dark);
    const QString cmtColor = Theme::colorToken(QStringLiteral("codeComment"), dark);
    const QString numColor = Theme::colorToken(QStringLiteral("codeNumber"), dark);

    QString out;
    const int n = code.size();
    int i = 0;

    while (i < n) {
        const QChar c = code.at(i);

        if (rules.hashComment && c == QLatin1Char('#')) {
            int end = code.indexOf(QLatin1Char('\n'), i);
            if (end < 0) end = n;
            out += span(cmtColor, code.mid(i, end - i).toHtmlEscaped());
            i = end;
            continue;
        }
        if (rules.slashSlashComment && c == QLatin1Char('/') && i + 1 < n && code.at(i + 1) == QLatin1Char('/')) {
            int end = code.indexOf(QLatin1Char('\n'), i);
            if (end < 0) end = n;
            out += span(cmtColor, code.mid(i, end - i).toHtmlEscaped());
            i = end;
            continue;
        }
        if (rules.dashDashComment && c == QLatin1Char('-') && i + 1 < n && code.at(i + 1) == QLatin1Char('-')) {
            int end = code.indexOf(QLatin1Char('\n'), i);
            if (end < 0) end = n;
            out += span(cmtColor, code.mid(i, end - i).toHtmlEscaped());
            i = end;
            continue;
        }
        if (rules.slashStarComment && c == QLatin1Char('/') && i + 1 < n && code.at(i + 1) == QLatin1Char('*')) {
            const int close = code.indexOf(QStringLiteral("*/"), i + 2);
            const int endPos = (close < 0) ? n : close + 2;
            out += span(cmtColor, code.mid(i, endPos - i).toHtmlEscaped());
            i = endPos;
            continue;
        }

        const bool atTripleQuote = rules.tripleQuoteString && i + 2 < n
            && (c == QLatin1Char('\'') || c == QLatin1Char('"'))
            && code.at(i + 1) == c && code.at(i + 2) == c;
        if (atTripleQuote) {
            const QString triple(3, c);
            const int close = code.indexOf(triple, i + 3);
            const int endPos = (close < 0) ? n : close + 3;
            out += span(strColor, code.mid(i, endPos - i).toHtmlEscaped());
            i = endPos;
            continue;
        }

        const bool atString = (rules.doubleQuoteString && c == QLatin1Char('"'))
            || (rules.singleQuoteString && c == QLatin1Char('\''))
            || (rules.backtickString && c == QLatin1Char('`'));
        if (atString) {
            const QChar quote = c;
            int j = i + 1;
            while (j < n && code.at(j) != quote) {
                if (code.at(j) == QLatin1Char('\\') && j + 1 < n)
                    j += 2;
                else
                    j++;
            }
            const int endPos = qMin(j + 1, n);
            out += span(strColor, code.mid(i, endPos - i).toHtmlEscaped());
            i = endPos;
            continue;
        }

        if (c.isDigit()) {
            int j = i;
            while (j < n && (code.at(j).isDigit() || code.at(j) == QLatin1Char('.')
                             || code.at(j) == QLatin1Char('x') || code.at(j) == QLatin1Char('X')
                             || (code.at(j).isLetter() && j > i && code.at(j - 1).isDigit())))
                j++;
            out += span(numColor, code.mid(i, j - i).toHtmlEscaped());
            i = j;
            continue;
        }

        if (isWordStart(c)) {
            int j = i;
            while (j < n && isWordChar(code.at(j)))
                j++;
            const QString word = code.mid(i, j - i);
            const bool isKeyword = rules.caseInsensitiveKeywords
                ? rules.keywords.contains(word.toUpper())
                : rules.keywords.contains(word);
            out += isKeyword ? span(kwColor, word.toHtmlEscaped()) : word.toHtmlEscaped();
            i = j;
            continue;
        }

        out += QString(c).toHtmlEscaped();
        i++;
    }
    return out;
}

// Highlights one complete tag, e.g. "<div class=\"foo\">" or "</div>" or
// "<br/>" — tagStr includes the leading '<' and trailing '>'.
QString highlightTag(const QString &tagStr, bool dark)
{
    const QString tagColor = Theme::colorToken(QStringLiteral("codeTag"), dark);
    const QString attrColor = Theme::colorToken(QStringLiteral("codeAttr"), dark);
    const QString strColor = Theme::colorToken(QStringLiteral("codeString"), dark);

    static const QRegularExpression re(
        QStringLiteral("^<(/?)([a-zA-Z!][a-zA-Z0-9:_-]*)((?:\\s[^<>]*)?)(/?)>$"),
        QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch m = re.match(tagStr);
    if (!m.hasMatch())
        return tagStr.toHtmlEscaped();

    QString out = QStringLiteral("&lt;") + m.captured(1);
    out += span(tagColor, m.captured(2).toHtmlEscaped());

    static const QRegularExpression attrRe(
        QStringLiteral("([a-zA-Z_:][a-zA-Z0-9_:.-]*)(?:(=)(\"[^\"]*\"|'[^']*'|[^\\s\"'<>]+))?"));
    QRegularExpressionMatchIterator it = attrRe.globalMatch(m.captured(3));
    while (it.hasNext()) {
        const QRegularExpressionMatch am = it.next();
        out += QLatin1Char(' ') + span(attrColor, am.captured(1).toHtmlEscaped());
        if (!am.captured(2).isEmpty()) {
            out += QLatin1Char('=');
            out += span(strColor, am.captured(3).toHtmlEscaped());
        }
    }
    out += m.captured(4) + QStringLiteral("&gt;");
    return out;
}

// Matches a complete, non-self-closing opening tag whose name is exactly
// "style" or "script" (e.g. "<script>" or "<script type=\"text/plain\">"),
// capturing the tag name so the caller can pick css vs. javascript rules.
const QRegularExpression &embeddedOpenTagRe()
{
    static const QRegularExpression re(
        QStringLiteral("^<(style|script)(?:\\s[^<>]*)?>$"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

QString highlightHtml(const QString &code, bool dark)
{
    const QString cmtColor = Theme::colorToken(QStringLiteral("codeComment"), dark);

    QString out;
    const int n = code.size();
    int i = 0;
    while (i < n) {
        if (code.mid(i, 4) == QStringLiteral("<!--")) {
            const int close = code.indexOf(QStringLiteral("-->"), i + 4);
            const int endPos = (close < 0) ? n : close + 3;
            out += span(cmtColor, code.mid(i, endPos - i).toHtmlEscaped());
            i = endPos;
            continue;
        }
        if (code.at(i) == QLatin1Char('<')) {
            const int close = code.indexOf(QLatin1Char('>'), i);
            const int endPos = (close < 0) ? n : close + 1;
            const QString tagStr = code.mid(i, endPos - i);
            out += highlightTag(tagStr, dark);
            i = endPos;

            const QRegularExpressionMatch openMatch = embeddedOpenTagRe().match(tagStr);
            if (openMatch.hasMatch()) {
                const QString tagName = openMatch.captured(1).toLower();
                const QString closeTag = QStringLiteral("</%1>").arg(tagName);
                const int closeStart = code.indexOf(closeTag, i, Qt::CaseInsensitive);
                const int contentEnd = (closeStart < 0) ? n : closeStart;
                bool found = false;
                const LangRules &rules = rulesFor(
                    tagName == QLatin1String("style") ? QStringLiteral("css") : QStringLiteral("javascript"),
                    &found);
                out += highlightGeneric(code.mid(i, contentEnd - i), rules, dark);
                i = contentEnd;
            }
            continue;
        }
        const int next = code.indexOf(QLatin1Char('<'), i);
        const int endPos = (next < 0) ? n : next;
        out += code.mid(i, endPos - i).toHtmlEscaped();
        i = endPos;
    }
    return out;
}

} // namespace

namespace CodeHighlighter {

QString highlightToHtml(const QString &code, const QString &language, bool dark)
{
    const QString canonical = canonicalLanguage(language);
    switch (kindFor(canonical)) {
    case Kind::Html:
        return highlightHtml(code, dark);
    case Kind::Generic: {
        bool found = false;
        const LangRules &rules = rulesFor(canonical, &found);
        return highlightGeneric(code, rules, dark);
    }
    case Kind::PlainOnly:
        return code.toHtmlEscaped();
    }
    return code.toHtmlEscaped();
}

bool isSupportedLanguage(const QString &language)
{
    return kindFor(canonicalLanguage(language)) != Kind::PlainOnly;
}

} // namespace CodeHighlighter
