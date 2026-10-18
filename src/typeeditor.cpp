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

#include "typeeditor.h"

#include <QKeyEvent>
#include <QPainter>
#include <QPalette>
#include <QSignalBlocker>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextOption>

TypeEditor::TypeEditor(QWidget *parent)
    : QTextEdit(parent)
{
    setLineWrapMode(QTextEdit::WidgetWidth);
    setAcceptRichText(false);
    // Tab moves on instead of inserting a tab: a QR payload rarely wants one,
    // and trapping the keyboard in the field is worse.
    setTabChangesFocus(true);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    connect(this, &QTextEdit::textChanged, this, &TypeEditor::ensureCenteredAlignment);
    ensureCenteredAlignment();

    // The empty-document placeholder is drawn from the document's default text
    // option, not the block format, so this is what centers it too.
    QTextOption option = document()->defaultTextOption();
    option.setAlignment(Qt::AlignHCenter);
    document()->setDefaultTextOption(option);
}

void TypeEditor::setCenteredPlaceholder(const QString &text)
{
    m_placeholder = text;
    // Qt's own placeholder is always drawn left aligned; paintEvent() draws
    // this one instead.
    setPlaceholderText(QString());
    viewport()->update();
}

void TypeEditor::ensureCenteredAlignment()
{
    bool centered = true;
    for (QTextBlock block = document()->begin(); block.isValid(); block = block.next()) {
        if (block.blockFormat().alignment() != Qt::AlignHCenter) {
            centered = false;
            break;
        }
    }
    if (centered) {
        return;
    }

    // The merge is a format change, not a text change; block the textChanged
    // it would re-emit so callers do not scan the document a second time.
    const QSignalBlocker blocker(this);
    QTextCursor cursor = textCursor();
    const int position = cursor.position();
    cursor.select(QTextCursor::Document);
    QTextBlockFormat format;
    format.setAlignment(Qt::AlignHCenter);
    cursor.mergeBlockFormat(format);
    cursor.setPosition(position);
    setTextCursor(cursor);
}

void TypeEditor::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if ((event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier)) != 0U) {
            insertPlainText(QStringLiteral("\n"));
        } else {
            Q_EMIT submitted();
        }
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        Q_EMIT cancelled();
        return;
    }
    QTextEdit::keyPressEvent(event);
}

void TypeEditor::paintEvent(QPaintEvent *event)
{
    QTextEdit::paintEvent(event);

    if (m_placeholder.isEmpty() || !document()->isEmpty()) {
        return;
    }
    QPainter painter(viewport());
    painter.setPen(palette().color(QPalette::PlaceholderText));
    const int margin = qRound(document()->documentMargin());
    const QRect area = viewport()->rect().adjusted(margin, margin, -margin, 0);
    painter.drawText(area, Qt::AlignHCenter | Qt::AlignTop, m_placeholder);
}
