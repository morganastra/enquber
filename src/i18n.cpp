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

#include "i18n.h"

#include <QCoreApplication>
#include <QTranslator>

namespace i18n {
namespace {

/// The translators live for the whole process: QCoreApplication does not take
/// ownership of a translator installed on it, and a later install() has to be
/// able to remove the previous pair before loading new catalogs.
QTranslator &englishCatalog()
{
    static QTranslator translator;
    return translator;
}

QTranslator &localeCatalog()
{
    static QTranslator translator;
    return translator;
}

} // namespace

bool install(const QLocale &locale)
{
    QCoreApplication::removeTranslator(&englishCatalog());
    QCoreApplication::removeTranslator(&localeCatalog());

    const bool english = englishCatalog().load(QLocale(QLocale::English), QStringLiteral("enquber"),
        QStringLiteral("_"), QStringLiteral(":/i18n"));
    if (english) {
        QCoreApplication::installTranslator(&englishCatalog());
    }

    // English and the C locale already resolve through the source catalog; a
    // second lookup would only add a redundant translator.
    if (locale.language() != QLocale::English
        && locale.language() != QLocale::C
        && localeCatalog().load(locale, QStringLiteral("enquber"), QStringLiteral("_"), QStringLiteral(":/i18n"))) {
        QCoreApplication::installTranslator(&localeCatalog());
    }

    return english;
}

} // namespace i18n
