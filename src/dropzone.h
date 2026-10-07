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
