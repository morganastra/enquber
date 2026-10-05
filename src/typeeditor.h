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

    bool m_aligning = false;
    QString m_placeholder;
};
