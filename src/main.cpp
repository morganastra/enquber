#include "i18n.h"
#include "mainwindow.h"
#include "theme.h"

#include <QApplication>
#include <QStyle>
#include <QStyleFactory>

namespace {

/// KDE ships the Breeze widget style, but without its platform theme Qt would
/// fall back to Fusion, and the application would look foreign on the desktop
/// it was started from. Prefer Breeze there, unless the user asked for a
/// specific style or Qt already picked Breeze.
void preferDesktopStyle()
{
    if (qEnvironmentVariableIsSet("QT_STYLE_OVERRIDE")) {
        return;
    }
    if (!qgetenv("XDG_CURRENT_DESKTOP").toLower().contains("kde")) {
        return;
    }
    if (QApplication::style()->objectName().compare(QLatin1String("breeze"), Qt::CaseInsensitive) == 0) {
        return;
    }
    const QStringList available = QStyleFactory::keys();
    if (!available.contains(QLatin1String("Breeze"), Qt::CaseInsensitive)) {
        return;
    }
    QApplication::setStyle(QStyleFactory::create(QLatin1String("Breeze")));
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("enquber"));
    QApplication::setApplicationVersion(QStringLiteral(ENQUBER_VERSION));
    QApplication::setOrganizationName(QStringLiteral("enquber"));
    QApplication::setDesktopFileName(QStringLiteral("enquber"));
    QApplication::setWindowIcon(theme::appIcon());
    preferDesktopStyle();

    if (!i18n::install(app)) {
        qWarning("enquber: the English translation catalog could not be loaded");
    }

    MainWindow window;

    // Interpret command line arguments as text to encode (and create QR code directly)
    const QStringList arguments = QApplication::arguments().mid(1);
    QStringList text;
    for (const QString &argument : arguments) {
        if (!argument.startsWith(QLatin1Char('-'))) {
            text.append(argument);
        }
    }
    if (!text.isEmpty()) {
        window.setText(text.join(QLatin1Char(' ')));
    }

    window.show();
    return QApplication::exec();
}
