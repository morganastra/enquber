#pragma once

#include <QIcon>
#include <QString>

#include <initializer_list>

namespace theme {

/// Returns the first icon of @p names that the current icon theme provides. 
/// If the theme does not provide action icons, we use the bundled fallback
/// glyph tinted with the system color theme palette foreground.
QIcon icon(std::initializer_list<const char *> names);

/// The application's own window icon, independent of the icon theme.
QIcon appIcon();

} // namespace theme
