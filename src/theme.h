#pragma once

#include <QIcon>

#include <initializer_list>

namespace theme {

/// Returns the first of @p names that resolves to an icon: the current icon
/// theme is preferred, and bundled fallback glyphs (tinted with the palette's
/// foreground) fill in when the theme cannot answer. Null when nothing matches.
QIcon icon(std::initializer_list<const char *> names);

/// The application's own window icon, independent of the icon theme.
QIcon appIcon();

} // namespace theme
