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

#include "qrcode.h"

#include <QImage>
#include <QPoint>
#include <QWidget>

class QMouseEvent;

/// Shows a QR symbol as large as it fits.
///
/// The symbol is rendered at a whole number of device pixels per module
/// whenever the size changes, so the modules never land on half pixels and the
/// code stays sharp and scannable at any window size or scaling factor.
class QrView : public QWidget
{
    Q_OBJECT

public:
    explicit QrView(QWidget *parent = nullptr);

    void setCode(const qr::Code &code);
    void clear();
    [[nodiscard]] bool hasCode() const { return m_code.isValid(); }

    /// Side, in logical pixels, of the symbol as it would be painted right now
    /// for the current widget size; 0 when there is nothing to draw. The inline
    /// editor is matched to this so the field lines up with the code above it.
    [[nodiscard]] int codeSide() const;

    /// Highlights the code; used while a drop is hovering over the window.
    void setHighlighted(bool highlighted);

    [[nodiscard]] QSize minimumSizeHint() const override;
    [[nodiscard]] QSize sizeHint() const override;

signals:
    /// Emitted when the user starts dragging the symbol. The owner builds the
    /// payload and runs the QDrag, because it knows how to export and name the
    /// image; the view only owns the gesture.
    void dragRequested();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    /// Device pixels per module at the current widget size: the largest whole
    /// number that fits. paintEvent() draws with it and codeSide() reports the
    /// result, so both have to read it from here. Needs a valid code.
    [[nodiscard]] int fittedModulePixels() const;
    void dropCache();

    qr::Code m_code;
    QImage m_cache;
    int m_cacheModulePixels = 0;
    qreal m_cacheDpr = 1.0;
    bool m_highlighted = false;

    QPoint m_pressPos;
    bool m_pressed = false;
};
