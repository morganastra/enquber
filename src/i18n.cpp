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

bool install(QCoreApplication &app, const QLocale &locale)
{
    app.removeTranslator(&englishCatalog());
    app.removeTranslator(&localeCatalog());

    const bool english = englishCatalog().load(QLocale(QLocale::English),
                                               QStringLiteral("enquber"),
                                               QStringLiteral("_"),
                                               QStringLiteral(":/i18n"));
    if (english) {
        app.installTranslator(&englishCatalog());
    }

    // English and the C locale already resolve through the source catalog; a
    // second lookup would only add a redundant translator.
    if (locale.language() != QLocale::English && locale.language() != QLocale::C) {
        if (localeCatalog().load(locale, QStringLiteral("enquber"), QStringLiteral("_"),
                                 QStringLiteral(":/i18n"))) {
            app.installTranslator(&localeCatalog());
        }
    }

    return english;
}

} // namespace i18n
