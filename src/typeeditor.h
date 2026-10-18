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

#include <QTextEdit>

/// The multi-line field used by the inline text-input feature. Return finishes
/// the edit, Ctrl+Return (or Shift+Return) inserts a newline, and Escape is
/// reported as a cancel rather than being typed.
///
/// It derives from QTextEdit rather than QPlainTextEdit because only the rich
/// text document layout honors paragraph alignment, which the field uses to
/// center its text. Rich input is turned off, so it still behaves as a plain
/// text field.
class TypeEditor : public QTextEdit
{
    Q_OBJECT

public:
    explicit TypeEditor(QWidget *parent = nullptr);

    /// Replaces Qt's placeholder (which is always drawn left aligned) with one
    /// that follows the centered layout.
    void setCenteredPlaceholder(const QString &text);

signals:
    void submitted();
    void cancelled();

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    /// Keeps every paragraph horizontally centered. Alignment lives in the block
    /// format, and setPlainText()/clear() reset it, so this runs from
    /// textChanged and does nothing once the blocks already match.
    void ensureCenteredAlignment();

    QString m_placeholder;
};
