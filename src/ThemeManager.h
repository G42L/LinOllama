#pragma once

#include <QObject>

// Resolves the user's theme preference (Light / Dark / follow-OS) to an
// actual light-or-dark stylesheet, applies it to the QApplication, and
// persists the preference via QSettings. In Auto mode, it also re-resolves
// live whenever the OS-level color scheme changes (Qt 6.5+), so flipping
// your desktop's day/night setting updates the app without a restart. On
// older Qt6 it still detects the OS scheme at startup/mode-change, just
// without that live callback.
class ThemeManager : public QObject
{
    Q_OBJECT

public:
    enum class Mode { Light, Dark, Auto };
    Q_ENUM(Mode)

    explicit ThemeManager(QObject *parent = nullptr);

    Mode mode() const { return m_mode; }
    void setMode(Mode mode);

    // Whichever theme is actually applied right now — in Auto mode this
    // reflects the OS at the time it was last resolved, not the mode itself.
    bool isDarkActive() const { return m_darkActive; }

    // Re-applies the stylesheet and notifies listeners (icons etc. that
    // recolor on themeChanged) without touching mode or light-vs-dark — for
    // when something the stylesheet depends on changes outside of that,
    // e.g. a custom "Application" accent color picked in Settings.
    // resolveAndApply() alone won't do this: it only emits themeChanged()
    // when light/dark actually flips, but re-applying the QSS after an
    // accent change needs every accent-driven icon to recolor too, exactly
    // like a real theme flip does.
    void notifyAppearanceChanged();

signals:
    // Emitted whenever the applied theme actually changes — either because
    // the user picked a different mode, or (in Auto) because the OS scheme
    // changed underneath us.
    void themeChanged();

private:
    void resolveAndApply();
    bool detectSystemDark() const;

    Mode m_mode = Mode::Auto;
    bool m_darkActive = false;
};
