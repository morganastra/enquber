#pragma once

#include "mimetext.h"

#include <QWidget>

class QLabel;
class QDragEnterEvent;
class QDragLeaveEvent;
class QDropEvent;

/// The rectangle in the middle of an empty window that invites the user to
/// drop a link on it.
class DropZone : public QWidget
{
    Q_OBJECT

public:
    explicit DropZone(QWidget *parent = nullptr);

    /// Draws the border in the highlight colour while a drop hovers over the
    /// window; the whole window is a drop target, this is just its face.
    void setActive(bool active);
    bool isActive() const { return m_active; }

signals:
    /// Emitted with the text of a dropped payload that is worth encoding.
    void textDropped(const QString &text);

protected:
    void paintEvent(QPaintEvent *event) override;
    void changeEvent(QEvent *event) override;

    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    void refreshIcon();

    QLabel *m_icon = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_hint = nullptr;
    bool m_active = false;

    /// What the drop currently offering itself is carrying.
    mime::Payload m_payload;
};
