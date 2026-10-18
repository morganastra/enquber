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
class QMouseEvent;

class DropZone : public QWidget
{
    Q_OBJECT

public:
    explicit DropZone(QWidget *parent = nullptr);

    /// Highlights the zone while a drop that can be encoded hovers over the
    /// window; the whole window is the drop target, this is just its face.
    void setActive(bool active);
    [[nodiscard]] bool isActive() const { return m_active; }

signals:
    /// The user asked to type: a click on the prompt (Ctrl+L is handled by the
    /// window, which talks to the same slot).
    void clicked();

protected:
    bool event(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void changeEvent(QEvent *event) override;

    void mousePressEvent(QMouseEvent *event) override;

private:
    void refreshIcon();

    QLabel *m_icon = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_hint = nullptr;
    bool m_active = false;
};
