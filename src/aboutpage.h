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

#include <QWidget>

class QLabel;

/// The help / info page: copyright, license notice and project links. It is
/// shown as a third page of the main window's stack rather than as a window of
/// its own, so Enquber stays a single-window application. Leaving it is done
/// with the corner button (which turns into a back arrow while this is up),
/// Esc, or Ctrl+H.
class AboutPage : public QWidget
{
    Q_OBJECT

public:
    explicit AboutPage(QWidget *parent = nullptr);

protected:
    bool event(QEvent *event) override;

private:
    void refreshIcon();

    QLabel *m_icon = nullptr;
};
