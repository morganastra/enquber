#pragma once

#include "mimetext.h"

#include <QWidget>

class QLabel;
class QDragEnterEvent;
class QDragLeaveEvent;
class QDropEvent;
class QMouseEvent;

class DropZone : public QWidget
{
    Q_OBJECT

public:
    explicit DropZone(QWidget *parent = nullptr);

    /// Note that the whole window is actually a drop target
    void setActive(bool active);
    [[nodiscard]] bool isActive() const { return m_active; }

signals:
    void textDropped(const QString &text);
    /// The user asked to type: a click on the prompt (Ctrl+L is handled by the
    /// window, which talks to the same slot).
    void clicked();

protected:
    void paintEvent(QPaintEvent *event) override;
    void changeEvent(QEvent *event) override;

    void mousePressEvent(QMouseEvent *event) override;

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

    mime::Payload m_payload;
};
