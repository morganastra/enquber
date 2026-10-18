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

#include <QLocale>

namespace i18n {

/// Installs the catalogs bundled under ":/i18n" on the running
/// QCoreApplication.
///
/// The English source catalog is always installed first, so every id resolves
/// and an unknown locale never shows a raw id; @p locale's catalog is then
/// installed when one is bundled, so it wins per string and English fills the
/// gaps. A later call replaces the previous catalogs.
///
/// @return true when the English source catalog was installed.
bool install(const QLocale &locale = QLocale::system());

} // namespace i18n
