#pragma once

#include <QIcon>
#include <QString>

#include <initializer_list>

namespace theme {

/// Returns the first icon of @p names that the current icon theme provides. If
/// the theme provides none of them (plain XDG sessions only ship hicolor, which
/// has no action icons) the matching glyph bundled with enquber is tinted with
/// the palette foreground and returned instead, so the widgets always show a
/// picture. Only when nothing at all matches is a null QIcon returned.
///
/// To keep startup from paying for guaranteed theme misses, the theme is probed
/// once with a single ubiquitous action name ("edit-copy"): when even that is
/// missing every other lookup is skipped and the bundled glyphs are used
/// directly. The trade-off is all-or-nothing per theme: a theme that happens to
/// lack the probe but provides another requested name is not asked for it and
/// falls back to the bundled glyph. The result is cached per icon-theme name and
/// re-probed when that name changes; a change to the icon search paths alone is
/// not noticed.
QIcon icon(std::initializer_list<const char *> names);

/// The application's own window icon, independent of the icon theme.
QIcon appIcon();

} // namespace theme
