#include "typeeditor.h"

#include <QKeyEvent>
#include <QPainter>
#include <QPalette>
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
    if (m_aligning) {
        return; // the merge below re-emits textChanged
    }

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

    m_aligning = true;
    QTextCursor cursor = textCursor();
    const int position = cursor.position();
    cursor.select(QTextCursor::Document);
    QTextBlockFormat format;
    format.setAlignment(Qt::AlignHCenter);
    cursor.mergeBlockFormat(format);
    cursor.setPosition(position);
    setTextCursor(cursor);
    m_aligning = false;
}

void TypeEditor::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier)) {
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
