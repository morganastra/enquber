/// A drag source used by the GUI smoke test.
///
/// It shows a window holding a payload; dragging from it with the left button
/// starts a real Qt (XDND) drag carrying text/plain and text/uri-list, which is
/// exactly what a browser does when a link is dragged onto another window.
/// That makes the drop in the application under test a genuine one instead of a
/// synthesised event.
///
///     dragsource --text https://example.com
///
/// With --with-app the real application window is shown in the same process and
/// the drop lands on it; the drag then goes through the whole QDrag, platform
/// and widget stack (target detection via XdndAware, drag moves, drop delivery)
/// and the tool reports what the application ended up encoding. Because source
/// and target share a process, the smoke test can assert on that report (the
/// drop action and the encoded text) as well as on the window.
///
/// Exit status is 0 when the drop was accepted with the copy action.

#include "i18n.h"
#include "mainwindow.h"

#include <QApplication>
#include <QDrag>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QTimer>
#include <QUrl>

#include <cstdarg>
#include <cstdio>

namespace {

void report(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    std::vfprintf(stdout, format, args);
    va_end(args);
    std::fflush(stdout);
}

} // namespace

class DragSource : public QLabel
{
public:
    DragSource(const QString &payload, MainWindow *app, bool keepRunning)
        : m_payload(payload)
        , m_app(app)
        , m_keepRunning(keepRunning)
    {
        setWindowTitle(QStringLiteral("dragsource"));
        setAlignment(Qt::AlignCenter);
        setWordWrap(true);
        setFixedSize(320, 160);
        setStyleSheet(QStringLiteral(
            "QLabel { background: palette(base); border: 2px solid palette(highlight);"
            " padding: 8px; font-size: 11pt; }"));
        setText(QStringLiteral("drag me\n\n%1").arg(payload));
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        m_origin = event->position().toPoint();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!(event->buttons() & Qt::LeftButton)) {
            return;
        }
        if ((event->position().toPoint() - m_origin).manhattanLength() < QApplication::startDragDistance()) {
            return;
        }

        auto *mime = new QMimeData;
        mime->setText(m_payload);
        const QUrl url(m_payload);
        if (url.isValid() && !url.scheme().isEmpty()) {
            mime->setUrls({url});
        }

        auto *drag = new QDrag(this);
        drag->setMimeData(mime);
        const Qt::DropAction action = drag->exec(Qt::CopyAction);
        report("drop action: %d\n", static_cast<int>(action));
        if (m_app) {
            // The caller keeps looking at the application window, so only
            // report. The drop event travels through the event loop, so let it
            // arrive before asking what was encoded.
            MainWindow *application = m_app;
            QTimer::singleShot(250, [application] {
                report("app text: %s\n", qPrintable(application->encodedText()));
            });
        } else if (!m_keepRunning) {
            QApplication::quit();
        }
    }

private:
    QString m_payload;
    MainWindow *m_app = nullptr;
    bool m_keepRunning = false;
    QPoint m_origin;
};

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    // The helper can host the real MainWindow, which reads its (translated)
    // strings at construction, so the catalog has to be installed first.
    i18n::install(app);

    QString payload;
    bool withApp = false;
    bool keepRunning = false;
    const QStringList arguments = QApplication::arguments();
    for (int i = 1; i < arguments.size(); ++i) {
        if (arguments.at(i) == QLatin1String("--text") && i + 1 < arguments.size()) {
            payload = arguments.at(++i);
        } else if (arguments.at(i) == QLatin1String("--with-app")) {
            withApp = true;
        } else if (arguments.at(i) == QLatin1String("--keep")) {
            // Stay around for more drags: a test can then drag repeatedly from
            // the same window without the window manager reshuffling the screen.
            keepRunning = true;
        }
    }
    if (payload.isEmpty()) {
        std::fprintf(stderr, "usage: dragsource --text <payload> [--with-app]\n");
        return 2;
    }

    MainWindow *window = nullptr;
    if (withApp) {
        window = new MainWindow;
        window->move(40, 40);
        window->show();
    }

    DragSource source(payload, window, keepRunning);
    source.move(withApp ? 700 : 0, withApp ? 700 : 0);
    source.show();
    return QApplication::exec();
}
