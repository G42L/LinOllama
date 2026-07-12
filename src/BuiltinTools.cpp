#include "BuiltinTools.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QDateTime>
#include <QLocale>
#include <cmath>

namespace BuiltinTools {

QJsonObject webSearchDefinition()
{
    QJsonObject params{
        {"type", "object"},
        {"properties", QJsonObject{
            {"query", QJsonObject{
                {"type", "string"},
                {"description", "The search query."}
            }}
        }},
        {"required", QJsonArray{"query"}}
    };
    QJsonObject function{
        {"name", kWebSearch},
        {"description", "Search Wikipedia for background/factual information on a topic. "
                         "Best for encyclopedic or historical questions, not for current events, "
                         "news, or anything requiring live/real-time data."},
        {"parameters", params}
    };
    return QJsonObject{{"type", "function"}, {"function", function}};
}

QJsonObject calculateDefinition()
{
    QJsonObject params{
        {"type", "object"},
        {"properties", QJsonObject{
            {"expression", QJsonObject{
                {"type", "string"},
                {"description", "A plain arithmetic expression, e.g. \"(12.5 + 7) * 3 / 2\". "
                                 "Supports + - * / % ^ and parentheses only — no variables or functions."}
            }}
        }},
        {"required", QJsonArray{"expression"}}
    };
    QJsonObject function{
        {"name", kCalculate},
        {"description", "Evaluate an arithmetic expression precisely. Use this instead of doing "
                         "arithmetic yourself whenever exact numeric correctness matters."},
        {"parameters", params}
    };
    return QJsonObject{{"type", "function"}, {"function", function}};
}

QJsonObject currentDateTimeDefinition()
{
    QJsonObject function{
        {"name", kCurrentDateTime},
        {"description", "Get the current local date and time. Use this for any question involving "
                         "\"today\", \"now\", \"what day is it\", relative dates, etc. — you have no "
                         "other way to know the current date."},
        {"parameters", QJsonObject{{"type", "object"}, {"properties", QJsonObject()}}}
    };
    return QJsonObject{{"type", "function"}, {"function", function}};
}

QJsonObject stackOverflowSearchDefinition()
{
    QJsonObject params{
        {"type", "object"},
        {"properties", QJsonObject{
            {"query", QJsonObject{
                {"type", "string"},
                {"description", "The search query, e.g. \"python reverse a list\" or an exact error message."}
            }}
        }},
        {"required", QJsonArray{"query"}}
    };
    QJsonObject function{
        {"name", kStackOverflowSearch},
        {"description", "Search Stack Overflow for programming questions and answers — error messages, "
                         "API usage, \"how do I...\" coding questions. Best for technical/programming "
                         "topics specifically, not general knowledge (use web_search for that instead)."},
        {"parameters", params}
    };
    return QJsonObject{{"type", "function"}, {"function", function}};
}

QString currentDateTimeText()
{
    return QLocale::system().toString(QDateTime::currentDateTime(), "dddd, yyyy-MM-dd HH:mm:ss t");
}

namespace {

// Tiny recursive-descent parser/evaluator for BuiltinTools::evaluateExpression().
// Grammar (standard precedence, ^ right-associative, unary minus above ^):
//   expr   := term (('+' | '-') term)*
//   term   := power (('*' | '/' | '%') power)*
//   power  := unary ('^' power)?
//   unary  := '-' unary | primary
//   primary:= NUMBER | '(' expr ')'
class ExpressionParser
{
public:
    explicit ExpressionParser(const QString &text) : m_text(text) {}

    // Throws QString error messages (caught by evaluateExpression()) rather
    // than threading a bool/error-string through every level — this parser
    // is only ever driven from evaluateExpression()'s own try/catch, never
    // called directly elsewhere.
    double parse()
    {
        const double result = parseExpr();
        skipSpaces();
        if (m_pos != m_text.length())
            throw QStringLiteral("unexpected character '%1'").arg(m_text[m_pos]);
        return result;
    }

private:
    double parseExpr()
    {
        double value = parseTerm();
        for (;;) {
            skipSpaces();
            if (peek() == '+') { ++m_pos; value += parseTerm(); }
            else if (peek() == '-') { ++m_pos; value -= parseTerm(); }
            else break;
        }
        return value;
    }

    double parseTerm()
    {
        double value = parseUnary();
        for (;;) {
            skipSpaces();
            const QChar c = peek();
            if (c == '*') { ++m_pos; value *= parseUnary(); }
            else if (c == '/') {
                ++m_pos;
                const double divisor = parseUnary();
                if (divisor == 0.0)
                    throw QStringLiteral("division by zero");
                value /= divisor;
            } else if (c == '%') {
                ++m_pos;
                const double divisor = parseUnary();
                if (divisor == 0.0)
                    throw QStringLiteral("division by zero");
                value = std::fmod(value, divisor);
            } else break;
        }
        return value;
    }

    // Deliberately binds *looser* than '^' (parsePower() is what unary
    // recurses into, not the other way around) — matches the conventional
    // calculator reading of "-3^2" as -(3^2) = -9, not (-3)^2 = 9. The
    // exponent itself can still be signed independently (e.g. "2^-3"),
    // since parsePower()'s own '^' branch recurses back into parseUnary().
    double parseUnary()
    {
        skipSpaces();
        if (peek() == '-') { ++m_pos; return -parseUnary(); }
        if (peek() == '+') { ++m_pos; return parseUnary(); }
        return parsePower();
    }

    double parsePower()
    {
        double value = parsePrimary();
        skipSpaces();
        if (peek() == '^') {
            ++m_pos;
            value = std::pow(value, parseUnary()); // right-associative; exponent may itself be signed
        }
        return value;
    }

    double parsePrimary()
    {
        skipSpaces();
        if (peek() == '(') {
            ++m_pos;
            const double value = parseExpr();
            skipSpaces();
            if (peek() != ')')
                throw QStringLiteral("missing closing parenthesis");
            ++m_pos;
            return value;
        }

        const int start = m_pos;
        if (peek() == '.' || peek().isDigit()) {
            while (m_pos < m_text.length() && (m_text[m_pos].isDigit() || m_text[m_pos] == '.'))
                ++m_pos;
            bool ok = false;
            const double value = m_text.mid(start, m_pos - start).toDouble(&ok);
            if (!ok)
                throw QStringLiteral("malformed number");
            return value;
        }

        throw peek().isNull() ? QStringLiteral("unexpected end of expression")
                               : QStringLiteral("unexpected character '%1'").arg(peek());
    }

    void skipSpaces()
    {
        while (m_pos < m_text.length() && m_text[m_pos].isSpace())
            ++m_pos;
    }

    QChar peek() const { return m_pos < m_text.length() ? m_text[m_pos] : QChar(); }

    QString m_text;
    int m_pos = 0;
};

} // namespace

QString evaluateExpression(const QString &expression)
{
    try {
        ExpressionParser parser(expression);
        const double result = parser.parse();
        // Whole numbers print without a trailing ".0" (nicer for a model to
        // read back), fractional ones keep enough precision to be exact for
        // any ordinary calculator-style question.
        if (result == std::floor(result) && std::abs(result) < 1e15)
            return QString::number(result, 'f', 0);
        return QString::number(result, 'g', 15);
    } catch (const QString &error) {
        return QStringLiteral("Error: %1").arg(error);
    }
}

ParsedToolCall parseToolCall(const QJsonObject &toolCallEntry)
{
    const QJsonObject function = toolCallEntry.value("function").toObject();

    ParsedToolCall parsed;
    parsed.name = function.value("name").toString();

    const QJsonValue argsValue = function.value("arguments");
    parsed.arguments = argsValue.toObject();
    if (parsed.arguments.isEmpty() && argsValue.isString())
        parsed.arguments = QJsonDocument::fromJson(argsValue.toString().toUtf8()).object();

    return parsed;
}

} // namespace BuiltinTools
