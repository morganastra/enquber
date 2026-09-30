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
QIcon icon(std::initializer_list<const char *> names);

/// The application's own window icon, independent of the icon theme.
QIcon appIcon();

} // namespace theme
