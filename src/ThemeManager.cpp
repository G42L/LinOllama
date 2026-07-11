#include "ThemeManager.h"
#include "Theme.h"

#include <QApplication>
#include <QSettings>
#include <QPalette>
#include <QtGlobal>

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
#include <QStyleHints>
#define OLLAMA_TRAY_HAS_COLOR_SCHEME_API 1
#endif

namespace {
constexpr auto kSettingsKey = "appearance/themeMode";
}

ThemeManager::ThemeManager(QObject *parent) : QObject(parent)
{
    const QSettings settings;
    const int stored = settings.value(kSettingsKey, static_cast<int>(Mode::Auto)).toInt();
    m_mode = static_cast<Mode>(stored);

#ifdef OLLAMA_TRAY_HAS_COLOR_SCHEME_API
    connect(qApp->styleHints(), &QStyleHints::colorSchemeChanged, this, [this](Qt::ColorScheme) {
        if (m_mode == Mode::Auto)
            resolveAndApply();
    });
#endif

    resolveAndApply();
}

void ThemeManager::setMode(Mode mode)
{
    if (m_mode == mode)
        return;
    m_mode = mode;

    QSettings settings;
    settings.setValue(kSettingsKey, static_cast<int>(mode));

    resolveAndApply();
}

bool ThemeManager::detectSystemDark() const
{
#ifdef OLLAMA_TRAY_HAS_COLOR_SCHEME_API
    const Qt::ColorScheme scheme = qApp->styleHints()->colorScheme();
    if (scheme != Qt::ColorScheme::Unknown)
        return scheme == Qt::ColorScheme::Dark;
#endif
    // Fallback for older Qt6 builds where colorScheme() isn't available:
    // guess from the base window color's lightness. Not live-updating, but
    // correct at startup and whenever the user re-selects Auto.
    return qApp->palette().color(QPalette::Window).lightness() < 128;
}

void ThemeManager::resolveAndApply()
{
    const bool wasDark = m_darkActive;
    m_darkActive = (m_mode == Mode::Dark) || (m_mode == Mode::Auto && detectSystemDark());

    qApp->setStyleSheet(Theme::styleSheet(m_darkActive));

    if (m_darkActive != wasDark)
        emit themeChanged();
}

void ThemeManager::notifyAppearanceChanged()
{
    qApp->setStyleSheet(Theme::styleSheet(m_darkActive));
    emit themeChanged();
}
