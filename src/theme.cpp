#include "theme.h"

#include <QColor>
#include <QGuiApplication>
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

/// Loads the bundled glyph for @p themeName and tints it, or a null QIcon when
/// there is no glyph for that name.
QIcon bundledIcon(const QString &themeName)
{
    const QImage source(
        QStringLiteral(":/enquber/icons/actions/%1.png").arg(bundledGlyphName(themeName)));
    if (source.isNull()) {
        return {};
    }

    const QColor color = foreground();
    QIcon icon;
    for (const int size : {16, 24, 32, 48, 64}) {
        const QImage scaled = source.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        icon.addPixmap(QPixmap::fromImage(tinted(scaled, color)));
    }
    return icon;
}

} // namespace

QIcon icon(std::initializer_list<const char *> names)
{
    for (const char *name : names) {
        const QIcon candidate = QIcon::fromTheme(QString::fromLatin1(name));
        if (!candidate.isNull()) {
            return candidate;
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
