#pragma once

#include <QString>
#include <QJsonObject>

// Definitions and local (non-web-search) execution for the app's built-in
// tool-calling tools — see ChatWidget's "Tools" menu and ToolExecutor. Kept
// as free functions/constants rather than a class since none of this needs
// any state between calls; ToolExecutor (which does need state, for the
// async web_search case) is what actually drives execution.
namespace BuiltinTools {

// Ollama's own tool_calls "function.name" values — used both when building
// the definitions below and when ToolExecutor dispatches an incoming call,
// so the two can never drift out of sync with each other.
constexpr const char *kWebSearch = "web_search";
constexpr const char *kCalculate = "calculate";
constexpr const char *kCurrentDateTime = "get_current_datetime";

// Each returns one entry of the /api/chat "tools" array (the
// {"type": "function", "function": {...}} wrapper Ollama expects) — see
// ChatWidget::buildToolDefinitions(), which assembles the subset the person
// has actually enabled via the Tools menu into one QJsonArray.
QJsonObject webSearchDefinition();
QJsonObject calculateDefinition();
QJsonObject currentDateTimeDefinition();

// Evaluates a plain arithmetic expression (+, -, *, /, %, ^, parentheses,
// unary minus, decimal numbers — no variables or functions, since the
// "calculate" tool exists for arithmetic the model itself is unreliable
// at, not as a general scripting engine). Returns the formatted numeric
// result, or a short human-readable error string starting with "Error:"
// on a malformed expression or division by zero — either way the return
// value is safe to hand straight back to the model as the tool's result
// text, no separate error path needed by the caller.
QString evaluateExpression(const QString &expression);

// Formats the local wall-clock date/time as ISO-ish text a model can parse
// and reason about reliably (e.g. weekday-relative questions like "what
// day is next Friday").
QString currentDateTimeText();

struct ParsedToolCall
{
    QString name;
    QJsonObject arguments;
};

// Extracts {name, arguments} from one entry of Ollama's tool_calls array
// (the {"function": {"name", "arguments"}} shape — toolCallEntry is one
// element of that array, not the array itself). Handles both an already-
// parsed "arguments" object (the normal case) and a JSON-encoded string
// (some model families emit it that way instead), so every caller —
// ToolExecutor's dispatch and ChatWidget's tool-call display — shares one
// place that knows about that quirk rather than each re-deriving it.
ParsedToolCall parseToolCall(const QJsonObject &toolCallEntry);

} // namespace BuiltinTools
