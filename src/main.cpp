#include "mainwindow.h"

#include <QApplication>
#include <QIcon>
#include <QStyle>
#include <QStyleFactory>

namespace {

/// KDE ships the Breeze widget style, but without its platform theme Qt would
/// fall back to Fusion, and the application would look foreign on the desktop
/// it was started from. Prefer Breeze there, unless the user asked for a
/// specific style or the platform theme already chose one.
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
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QApplication::setOrganizationName(QStringLiteral("enquber"));
    QApplication::setDesktopFileName(QStringLiteral("enquber"));
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("view-barcode-qr")));
    preferDesktopStyle();

    MainWindow window;

    // `enquber https://example.com` shows the code right away; anything that was
    // not meant for Qt itself counts as the text to encode.
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
