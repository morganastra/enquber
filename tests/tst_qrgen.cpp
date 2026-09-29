#include "dropzone.h"
#include "mainwindow.h"
#include "mimetext.h"
#include "qrcode.h"
#include "qrview.h"

#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <QStackedWidget>
#include <QDir>
#include <QFileDialog>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QTest>

namespace {

void sendDragEnter(QWidget *target, const QMimeData *mime)
{
    QDragEnterEvent event(target->rect().center(), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(target, &event);
}

/// Qt only delivers a drop that follows a drag enter, exactly like a real
/// drag does.
void performDrop(QWidget *target, const QMimeData *mime)
{
    sendDragEnter(target, mime);
    QDropEvent event(QPointF(target->rect().center()), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(target, &event);
}

/// Shows the window and makes it the active one, which is what keyboard
/// shortcuts are scoped to.
void showAndActivate(QWidget *window)
{
    window->show();
    window->activateWindow();
    QCoreApplication::processEvents();
}

QMimeData *textMime(const QString &text)
{
    auto *mime = new QMimeData;
    mime->setText(text);
    return mime;
}

/// The offscreen clipboard hands back a null QMimeData when it is empty, so
/// every clipboard assertion goes through this.
bool clipboardHasImage()
{
    const QMimeData *mime = QGuiApplication::clipboard()->mimeData();
    return mime && mime->hasImage();
}

QPushButton *buttonContaining(QWidget *window, const QString &needle)
{
    const QList<QPushButton *> buttons = window->findChildren<QPushButton *>();
    for (QPushButton *button : buttons) {
        if (button->text().contains(needle, Qt::CaseInsensitive)) {
            return button;
        }
    }
    return nullptr;
}

} // namespace

class TestQrGen : public QObject
{
    Q_OBJECT

private slots:
    void encodesText_data();
    void encodesText();

    void rejectsEmptyText();
    void rejectsOversizedText();

    void rendersQuietZoneAndModules();
    void picksSmallestVersion();

    void dropOnWindowShowsCode();
    void dropOnDropZoneShowsCode();
    void droppingUrlsPrefersTheUrl();
    void payloadSurvivesASourceThatStopsAnswering();
    void dropWithoutTextIsIgnored();

    void pasteEncodesClipboardText();
    void pasteWithoutTextIsIgnored();

    void copyPutsImageOnClipboard();
    void clearingReturnsToTheDropTarget();
    void clearingWithTheButtonWorks();
    void droppingStillWorksAfterClearing();
    void clickingCopyButtonCopiesImage();
    void keyboardReachesTheButtons();
    void clickingSaveButtonWritesFile();
    void saveWritesPngFile();
    void saveAppendsPngSuffix();

    void windowAcceptsDrops();
};

void TestQrGen::encodesText_data()
{
    QTest::addColumn<QString>("text");
    QTest::newRow("url") << QStringLiteral("https://example.com/some/path?a=1&b=2");
    QTest::newRow("plain") << QStringLiteral("hello world");
    QTest::newRow("unicode") << QStringLiteral("Grüße aus München ✓");
    QTest::newRow("multiline") << QStringLiteral("first line\nsecond line");
}

void TestQrGen::encodesText()
{
    QFETCH(QString, text);

    const qr::Code code = qr::Code::encode(text);

    QVERIFY(code.isValid());
    QVERIFY(code.error().isEmpty());
    QCOMPARE(code.text(), text);
    QVERIFY(code.modules() >= 21);
}

void TestQrGen::rejectsEmptyText()
{
    const qr::Code code = qr::Code::encode(QString());

    QVERIFY(!code.isValid());
    QVERIFY(!code.error().isEmpty());
    QVERIFY(code.toImage(4).isNull());
}

void TestQrGen::rejectsOversizedText()
{
    const qr::Code code = qr::Code::encode(QString(5000, QLatin1Char('a')));

    QVERIFY(!code.isValid());
    QVERIFY(!code.error().isEmpty());
}

void TestQrGen::rendersQuietZoneAndModules()
{
    const qr::Code code = qr::Code::encode(QStringLiteral("https://example.com"));
    QVERIFY(code.isValid());

    constexpr int modulePixels = 3;
    const QImage image = code.toImage(modulePixels);
    const int modules = code.modules();

    QCOMPARE(image.size(), QSize((modules + 8) * modulePixels, (modules + 8) * modulePixels));
    QCOMPARE(image.format(), QImage::Format_RGB32);

    // The quiet zone is white all around...
    QCOMPARE(image.pixelColor(0, 0), QColor(Qt::white));
    QCOMPARE(image.pixelColor(image.width() - 1, 0), QColor(Qt::white));
    QCOMPARE(image.pixelColor(0, image.height() - 1), QColor(Qt::white));

    // ...and the top left corner of the finder pattern is black.
    QCOMPARE(image.pixelColor(4 * modulePixels, 4 * modulePixels), QColor(Qt::black));

    // Every module maps to a solid block of pixels.
    for (int y = 0; y < modules; ++y) {
        for (int x = 0; x < modules; ++x) {
            const QColor expected = code.isDark(x, y) ? QColor(Qt::black) : QColor(Qt::white);
            const QColor actual = image.pixelColor((x + 4) * modulePixels, (y + 4) * modulePixels);
            if (actual != expected) {
                QFAIL(qPrintable(QStringLiteral("module %1,%2 is %3").arg(x).arg(y).arg(actual.name())));
            }
        }
    }
}

void TestQrGen::picksSmallestVersion()
{
    const qr::Code small = qr::Code::encode(QStringLiteral("hi"));
    const qr::Code large = qr::Code::encode(QString(1000, QLatin1Char('a')));

    QVERIFY(small.isValid());
    QVERIFY(large.isValid());
    QVERIFY(small.modules() < large.modules());
}

void TestQrGen::dropOnWindowShowsCode()
{
    MainWindow window;
    showAndActivate(&window);
    QVERIFY(!window.hasCode());

    QScopedPointer<QMimeData> mime(textMime(QStringLiteral("https://drop.example")));
    performDrop(&window, mime.data());

    QVERIFY(window.hasCode());
    QCOMPARE(window.encodedText(), QStringLiteral("https://drop.example"));
    QVERIFY(window.findChild<QrView *>()->hasCode());
}

void TestQrGen::dropOnDropZoneShowsCode()
{
    MainWindow window;
    showAndActivate(&window);
    auto *zone = window.findChild<DropZone *>();
    QVERIFY(zone);

    QScopedPointer<QMimeData> mime(textMime(QStringLiteral("https://zone.example")));
    sendDragEnter(zone, mime.data());
    QVERIFY(zone->isActive());
    performDrop(zone, mime.data());

    QVERIFY(!zone->isActive());
    QVERIFY(window.hasCode());
    QCOMPARE(window.encodedText(), QStringLiteral("https://zone.example"));
}

void TestQrGen::droppingUrlsPrefersTheUrl()
{
    auto *mime = new QMimeData;
    mime->setUrls({QUrl(QStringLiteral("https://url.example/x"))});
    mime->setText(QStringLiteral("https://text.example/y"));

    QCOMPARE(mime::textForQr(mime), QStringLiteral("https://url.example/x"));
    delete mime;
}

void TestQrGen::payloadSurvivesASourceThatStopsAnswering()
{
    // On X11 the payload is fetched from the drag source when the target asks
    // for it, and a source may refuse once the button is released. What was
    // readable while hovering has to be enough to complete the drop.
    QScopedPointer<QMimeData> hovering(textMime(QStringLiteral("https://slow-source.example")));
    mime::Payload payload;
    QCOMPARE(payload.observe(hovering.data()), QStringLiteral("https://slow-source.example"));

    auto *mute = new QMimeData; // formats advertised, but no data behind them
    mute->setData(QStringLiteral("text/plain"), QByteArray());
    QVERIFY(mute->hasFormat(QStringLiteral("text/plain")));
    QCOMPARE(payload.resolve(mute), QStringLiteral("https://slow-source.example"));

    // A payload that does answer wins over the observation.
    QScopedPointer<QMimeData> answering(textMime(QStringLiteral("https://fast-source.example")));
    QCOMPARE(payload.resolve(answering.data()), QStringLiteral("https://fast-source.example"));

    // And once the drag is over the observation is gone.
    payload.forget();
    QVERIFY(payload.resolve(mute).isEmpty());
    delete mute;
}

void TestQrGen::dropWithoutTextIsIgnored()
{
    MainWindow window;
    showAndActivate(&window);

    auto *mime = new QMimeData;
    mime->setHtml(QStringLiteral("<b>not text</b>"));
    performDrop(&window, mime);
    delete mime;

    QVERIFY(!window.hasCode());
    QCOMPARE(window.findChild<QStackedWidget *>()->currentIndex(), 0);
}

void TestQrGen::pasteEncodesClipboardText()
{
    MainWindow window;
    showAndActivate(&window);
    QCOMPARE(QApplication::activeWindow(), &window);
    QGuiApplication::clipboard()->setText(QStringLiteral("  https://paste.example  \n"));

    QTest::keyClick(&window, Qt::Key_V, Qt::ControlModifier);

    QVERIFY(window.hasCode());
    QCOMPARE(window.encodedText(), QStringLiteral("https://paste.example"));
}

void TestQrGen::pasteWithoutTextIsIgnored()
{
    MainWindow window;
    showAndActivate(&window);
    QGuiApplication::clipboard()->clear();

    QTest::keyClick(&window, Qt::Key_V, Qt::ControlModifier);

    QVERIFY(!window.hasCode());
}

void TestQrGen::copyPutsImageOnClipboard()
{
    MainWindow window;
    showAndActivate(&window);
    window.setText(QStringLiteral("https://clip.example"));

    QTest::keyClick(&window, Qt::Key_C, Qt::ControlModifier);

    const QMimeData *mime = QGuiApplication::clipboard()->mimeData();
    QVERIFY(mime);
    QVERIFY(mime->hasImage());
    QVERIFY(mime->hasFormat(QStringLiteral("image/png")));

    const QImage image = QGuiApplication::clipboard()->image();
    QVERIFY(!image.isNull());
    QVERIFY(image.width() > 500);
    QCOMPARE(image.width(), image.height());
    QVERIFY(image.pixelColor(0, 0) == QColor(Qt::white));
}

void TestQrGen::clearingReturnsToTheDropTarget()
{
    MainWindow window;
    showAndActivate(&window);
    window.setText(QStringLiteral("https://clear.example"));
    QVERIFY(window.hasCode());

    QTest::keyClick(&window, Qt::Key_Escape);

    QVERIFY(!window.hasCode());
    QVERIFY(window.encodedText().isEmpty());
    QCOMPARE(window.findChild<QStackedWidget *>()->currentIndex(), 0);
    QVERIFY(!window.findChild<QrView *>()->hasCode());
    auto *zone = window.findChild<DropZone *>();
    QVERIFY(zone->isVisible());

    // Nothing to export any more.
    QVERIFY(!window.saveTo(QDir::tempPath() + QStringLiteral("/should-not-exist.png")));
}

void TestQrGen::clearingWithTheButtonWorks()
{
    MainWindow window;
    showAndActivate(&window);
    window.setText(QStringLiteral("https://clear-button.example"));

    QPushButton *clear = buttonContaining(&window, QStringLiteral("lear"));
    QVERIFY(clear);
    QVERIFY(clear->isVisible());
    QTest::mouseClick(clear, Qt::LeftButton);

    QVERIFY(!window.hasCode());
    QCOMPARE(window.findChild<QStackedWidget *>()->currentIndex(), 0);
}

void TestQrGen::droppingStillWorksAfterClearing()
{
    MainWindow window;
    showAndActivate(&window);
    window.setText(QStringLiteral("https://first.example"));
    QTest::keyClick(&window, Qt::Key_Escape);
    QVERIFY(!window.hasCode());

    // The drop target has to come back to life, not just come back.
    QScopedPointer<QMimeData> mime(textMime(QStringLiteral("https://second.example")));
    performDrop(&window, mime.data());

    QVERIFY(window.hasCode());
    QCOMPARE(window.encodedText(), QStringLiteral("https://second.example"));
}

void TestQrGen::clickingCopyButtonCopiesImage()
{
    MainWindow window;
    showAndActivate(&window);
    window.setText(QStringLiteral("https://button.example"));
    QGuiApplication::clipboard()->clear();

    QPushButton *copy = buttonContaining(&window, QStringLiteral("copy"));
    QVERIFY(copy);
    QVERIFY(copy->isVisible());
    QTest::mouseClick(copy, Qt::LeftButton);

    QVERIFY(clipboardHasImage());
    const QImage image = QGuiApplication::clipboard()->image();
    QCOMPARE(image.width(), image.height());
}

void TestQrGen::keyboardReachesTheButtons()
{
    MainWindow window;
    showAndActivate(&window);
    window.setText(QStringLiteral("https://keyboard.example"));
    QGuiApplication::clipboard()->clear();

    QPushButton *copy = buttonContaining(&window, QStringLiteral("copy"));
    QVERIFY(copy);
    // Showing a code moves the keyboard to the action most people want next.
    QCOMPARE(QApplication::focusWidget(), copy);

    // Space has to go to the focused widget: Qt delivers key events to the
    // focus widget, not to the window.
    QTest::keyClick(copy, Qt::Key_Space);
    QVERIFY(clipboardHasImage());

    // Tabbing from the window itself walks to the copy button first.
    window.setFocus();
    QTest::keyClick(&window, Qt::Key_Tab);
    QCOMPARE(QApplication::focusWidget(), copy);
    QTest::keyClick(&window, Qt::Key_Tab);
    QPushButton *save = buttonContaining(&window, QStringLiteral("save"));
    QCOMPARE(QApplication::focusWidget(), save);
}

void TestQrGen::clickingSaveButtonWritesFile()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    MainWindow window;
    showAndActivate(&window);
    window.setText(QStringLiteral("https://savebutton.example"));

    const QString path = directory.filePath(QStringLiteral("from-button.png"));
    QTimer::singleShot(0, &window, [&window, path] {
        auto *dialog = window.findChild<QFileDialog *>();
        QVERIFY(dialog);
        dialog->selectFile(path);
        // accept() is protected, but still a slot, so go through the meta object.
        QVERIFY(QMetaObject::invokeMethod(dialog, "accept"));
    });

    QPushButton *save = buttonContaining(&window, QStringLiteral("save"));
    QVERIFY(save);
    QTest::mouseClick(save, Qt::LeftButton);

    QVERIFY(QFileInfo::exists(path));
    QVERIFY(!QImage(path).isNull());
}

void TestQrGen::saveWritesPngFile()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    MainWindow window;
    showAndActivate(&window);
    window.setText(QStringLiteral("https://save.example"));

    const QString path = directory.filePath(QStringLiteral("code.png"));
    QVERIFY(window.saveTo(path));

    QVERIFY(QFileInfo::exists(path));
    const QImage image(path);
    QVERIFY(!image.isNull());
    QCOMPARE(image.width(), image.height());
    QVERIFY(image.width() > 500);
}

void TestQrGen::saveAppendsPngSuffix()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    MainWindow window;
    showAndActivate(&window);
    window.setText(QStringLiteral("https://save.example"));

    const QString path = directory.filePath(QStringLiteral("without-suffix"));
    QVERIFY(window.saveTo(path));

    QVERIFY(QFileInfo::exists(path + QStringLiteral(".png")));
}

void TestQrGen::windowAcceptsDrops()
{
    MainWindow window;
    QVERIFY(window.acceptDrops());
    QVERIFY(window.findChild<DropZone *>()->acceptDrops());
    QVERIFY(window.minimumSizeHint().isValid());
}

QTEST_MAIN(TestQrGen)
#include "tst_qrgen.moc"
