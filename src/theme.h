#pragma once

#include <QIcon>
#include <QString>

#include <initializer_list>

namespace theme {

/// Returns the first icon of @p names that the current icon theme provides, or
/// a null QIcon when none of them exist. Widgets fall back to their text when
/// the icon is null, so a theme that ships nothing still looks deliberate.
QIcon icon(std::initializer_list<const char *> names);

} // namespace theme
