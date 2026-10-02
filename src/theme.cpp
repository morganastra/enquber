#include "theme.h"

#include <QColor>
#include <QGuiApplication>
#include <QHash>
#include <QIcon>
#include <QImage>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QString>

#include <initializer_list>

namespace theme {
namespace {

/// Maps a freedesktop icon name onto the bundled glyph that stands in for it.
/// Names Qt treats as the same action share one picture (saving and saving as,
/// for example), and everything without a bundled glyph is returned unchanged
/// so that the resource lookup simply comes up empty.
QString bundledGlyphName(const QString &themeName)
{
    if (themeName == QLatin1String("document-save-as")) {
        return QStringLiteral("document-save");
    }
    if (themeName == QLatin1String("edit-clear-all") || themeName == QLatin1String("window-close")) {
        return QStringLiteral("edit-clear");
    }
    if (themeName == QLatin1String("edit-paste") || themeName == QLatin1String("document-open")) {
        return QStringLiteral("insert-link");
    }
    if (themeName == QLatin1String("go-previous") || themeName == QLatin1String("go-previous-symbolic")) {
        return QStringLiteral("arrow-left");
    }
    // The "?" and the "i" are easy to confuse in the freedesktop naming: the
    // contents/help names are the question mark, while help-about is the info.
    if (themeName == QLatin1String("help") || themeName == QLatin1String("help-contents")
        || themeName == QLatin1String("help-browser") || themeName == QLatin1String("system-help")) {
        return QStringLiteral("help-circle");
    }
    if (themeName == QLatin1String("help-about") || themeName == QLatin1String("dialog-information")
        || themeName == QLatin1String("dialog-info")) {
        return QStringLiteral("info");
    }
    return themeName;
}

/// The colour a fallback glyph should take: the application's foreground, so
/// that one black-on-transparent source reads well on light and dark palettes.
QColor foreground()
{
    const QColor color = QGuiApplication::palette().color(QPalette::Active, QPalette::WindowText);
    return color.isValid() ? color : QColor(Qt::black);
}

/// Recolours a monochrome image while keeping its alpha channel, which is what
/// turns the black source glyph into one matching the palette.
QImage tinted(const QImage &source, const QColor &color)
{
    QImage result = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&result);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(result.rect(), color);
    painter.end();
    return result;
}

/// Upper bound on the bundled-glyph cache. There are only a handful of distinct
/// glyphs and, in practice, one or two foreground colours (a light and a dark
/// palette), so this only ever trips after a long run of palette changes.
constexpr int kMaxCachedGlyphs = 16;

/// Loads the bundled glyph for @p themeName and tints it, or a null QIcon when
/// there is no glyph for that name.
///
/// Scaling and tinting five sizes is the expensive part and a palette change
/// asks for the same pictures again in a new colour, so the finished icons are
/// kept in a small cache keyed on the resolved glyph name and the exact tint.
/// A different colour never reuses another one's entry.
QIcon bundledIcon(const QString &themeName)
{
    const QString glyph = bundledGlyphName(themeName);
    const QColor color = foreground();
    const QString key = glyph + QLatin1Char(':') + QString::number(color.rgba());

    static QHash<QString, QIcon> cache;
    if (const auto cached = cache.constFind(key); cached != cache.constEnd()) {
        return cached.value();
    }

    const QImage source(QStringLiteral(":/enquber/icons/actions/%1.png").arg(glyph));
    if (source.isNull()) {
        return {};
    }

    QIcon icon;
    for (const int size : {16, 24, 32, 48, 64}) {
        const QImage scaled = source.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        icon.addPixmap(QPixmap::fromImage(tinted(scaled, color)));
    }

    if (cache.size() >= kMaxCachedGlyphs) {
        cache.clear();
    }
    cache.insert(key, icon);
    return icon;
}

/// Whether the active icon theme can resolve the action icons enquber uses.
///
/// A plain XDG session only ships the HiColor theme, which has no action icons,
/// so every QIcon::fromTheme() call is a guaranteed miss. Checking one ubiquitous
/// name once and caching the answer turns a dozen of those failures into a single
/// one. The check is deliberately all-or-nothing: a theme that lacks the probe
/// ("edit-copy", which every real icon theme provides) is assumed to lack the
/// rest too, and enquber's bundled glyphs are used without asking again. A theme
/// that has the probe still goes through the normal per-name lookup.
///
/// The answer is tied to the theme name it was probed under, so switching icon
/// themes at runtime re-probes. An empty theme name is treated as "not settled
/// yet" (the probe can run before the platform theme is applied) and is never
/// cached, so an early miss cannot stick around for the rest of the process.
bool themeProvidesActionIcons()
{
    const QString themeName = QIcon::themeName();
    static QString probedThemeName;
    static bool cachedResult = false;
    static bool hasCachedResult = false;

    if (hasCachedResult && themeName == probedThemeName) {
        return cachedResult;
    }

    const bool providesIcons = !QIcon::fromTheme(QStringLiteral("edit-copy")).isNull();

    if (!themeName.isEmpty()) {
        probedThemeName = themeName;
        cachedResult = providesIcons;
        hasCachedResult = true;
    }
    return providesIcons;
}

} // namespace

QIcon icon(std::initializer_list<const char *> names)
{
    // Only ask the icon theme when it is worth it: on a plain session every one
    // of these calls would fail, and the bundled glyphs below can answer just as
    // well. See themeProvidesActionIcons() for the trade-off.
    if (themeProvidesActionIcons()) {
        for (const char *name : names) {
            const QIcon candidate = QIcon::fromTheme(QString::fromLatin1(name));
            if (!candidate.isNull()) {
                return candidate;
            }
        }
    }

    // No system theme answered; fall back to the glyphs shipped with enquber.
    for (const char *name : names) {
        const QIcon candidate = bundledIcon(QString::fromLatin1(name));
        if (!candidate.isNull()) {
            return candidate;
        }
    }
    return {};
}

QIcon appIcon()
{
    return QIcon(QStringLiteral(":/enquber/icons/enquber.png"));
}

} // namespace theme
