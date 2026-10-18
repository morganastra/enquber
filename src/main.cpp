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
    // Qt's application object is the process singleton; it stays non-const
    // even though only its static members are called here.
    // NOLINTNEXTLINE(misc-const-correctness)
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("enquber"));
    QApplication::setApplicationVersion(QStringLiteral(ENQUBER_VERSION));
    QApplication::setOrganizationName(QStringLiteral("enquber"));
    QApplication::setDesktopFileName(QStringLiteral("enquber"));
    QApplication::setWindowIcon(theme::appIcon());
    preferDesktopStyle();

    if (!i18n::install()) {
        qWarning("enquber: the English translation catalog could not be loaded");
    }

    MainWindow window;

    // `enquber https://example.com` shows the code right away: every argument
    // that does not start with '-' is text to encode.
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
