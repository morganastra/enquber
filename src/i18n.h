#pragma once

#include <QLocale>

class QCoreApplication;

namespace i18n {

/// Installs the catalogs bundled under ":/i18n" on @p app.
///
/// The English source catalog is always installed first, so every id resolves
/// and an unknown locale never shows a raw id; @p locale's catalog is then
/// installed when one is bundled, so it wins per string and English fills the
/// gaps. A later call replaces the previous catalogs.
///
/// @return true when the English source catalog was installed.
bool install(QCoreApplication &app, const QLocale &locale = QLocale::system());

} // namespace i18n
