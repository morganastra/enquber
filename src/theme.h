// enquber - a simple cross-platform QR code maker
// Copyright (C) 2026 Morgan Astra
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

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
