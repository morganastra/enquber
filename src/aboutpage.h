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
