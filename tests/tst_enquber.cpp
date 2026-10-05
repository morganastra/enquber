#include "dropzone.h"
#include "i18n.h"
#include "mainwindow.h"
#include "mimeimage.h"
#include "mimetext.h"
#include "qrcode.h"
#include "qrview.h"
#include "theme.h"

#include <QApplication>
#include <QAbstractButton>
#include <QAction>
#include <QByteArray>
#include <QClipboard>
#include <QColor>
#include <QFileInfo>
#include <QKeySequence>
#include <QMimeData>
#include <QMouseEvent>
#include <QPalette>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QIcon>
#include <QLabel>
#include <QLocale>
#include <QPixmap>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
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

/// Sends the press and move that begin dragging from @p target. The move
/// carries the held button explicitly, unlike QTest::mouseMove(), which sends a
/// move with no button and would not reach an ordinary widget.
void sendDragGesture(QWidget *target, const QPoint &from, const QPoint &to, bool hold = true)
{
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(from), target->mapToGlobal(QPointF(from)),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(target, &press);

    const Qt::MouseButtons buttons = hold ? Qt::LeftButton : Qt::NoButton;
    QMouseEvent move(QEvent::MouseMove, QPointF(to), target->mapToGlobal(QPointF(to)),
                     Qt::NoButton, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(target, &move);
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

/// The color of a strongly opaque pixel of a tinted glyph; the source is
/// recolored through SourceIn, so every opaque pixel carries the tint.
QColor inkOf(const QImage &image)
{
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor color = image.pixelColor(x, y);
            if (color.alpha() > 200) {
                return color;
            }
        }
    }
    return {};
}

/// The color @p widget will draw its text in, following the palette role it
/// asked for. Checking the role like this is more robust than reading pixels,
/// because a small glyph may never produce a fully opaque pixel.
QColor drawnTextColor(const QWidget *widget)
{
    return widget->palette().color(widget->foregroundRole());
}

/// Writes a minimal freedesktop icon theme under @p root named @p themeName
/// that provides exactly one action icon, @p iconName, filled with @p color.
/// The probe test uses this to drive the theme branch without depending on the
/// host's installed icon themes. Returns false when anything could not be
/// written.
bool writeIconTheme(const QString &root, const QString &themeName,
                    const QString &iconName, const QColor &color)
{
    const QString directory = QDir(root).filePath(themeName);
    if (!QDir().mkpath(directory + QStringLiteral("/16x16/actions"))) {
        return false;
    }

    QFile index(directory + QStringLiteral("/index.theme"));
    if (!index.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    const QString contents =
        QStringLiteral("[Icon Theme]\n"
                       "Name=%1\n"
                       "Directories=16x16/actions\n"
                       "\n"
                       "[16x16/actions]\n"
                       "Size=16\n"
                       "Type=Fixed\n"
                       "Context=Actions\n")
            .arg(themeName);
    index.write(contents.toUtf8());
    index.close();

    QImage marker(16, 16, QImage::Format_ARGB32_Premultiplied);
    marker.fill(color);
    return marker.save(directory + QStringLiteral("/16x16/actions/") + iconName
                           + QStringLiteral(".png"),
                       "PNG");
}

/// Puts the global icon-theme settings back when it goes out of scope, so a
/// QVERIFY that fails part way through a test cannot leak a synthetic theme into
/// the tests that follow. QVERIFY returns from the test function, which runs the
/// guard.
class ThemeStateRestorer
{
public:
    ThemeStateRestorer()
        : m_paths(QIcon::themeSearchPaths())
        , m_name(QIcon::themeName())
        , m_fallback(QIcon::fallbackThemeName())
    {
    }

    ~ThemeStateRestorer()
    {
        QIcon::setThemeSearchPaths(m_paths);
        QIcon::setFallbackThemeName(m_fallback);
        QIcon::setThemeName(m_name);
    }

    ThemeStateRestorer(const ThemeStateRestorer &) = delete;
    ThemeStateRestorer &operator=(const ThemeStateRestorer &) = delete;

private:
    QStringList m_paths;
    QString m_name;
    QString m_fallback;
};

} // namespace

class TestEnquber : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void initTestCase();
    void cleanupTestCase();

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
    void draggingOffersImageAndFile();
    void qrViewStartsDragOnGesture();
    void draggingBelowTheThresholdDoesNothing();
    void draggingWithoutACodeDoesNothing();
    void longTextIsShapedForTheLabel();
    void repeatingAStatusMessageRestartsItsTimeout();
    void clearingReturnsToTheDropTarget_data();
    void clearingReturnsToTheDropTarget();
    void clearingWithTheButtonWorks();
    void droppingStillWorksAfterClearing();
    void clickingCopyButtonCopiesImage();
    void keyboardReachesTheButtons();
    void clickingSaveButtonWritesFile();
    void saveWritesPngFile();
    void saveAppendsPngSuffix();
    void quitShortcutClosesWindow();

    void windowAcceptsDrops();

    void buttonsCarryIcons();
    void dropZoneCarriesAnIcon();
    void windowUsesTheBundledAppIcon();
    void fallbackIconsFollowThePalette();
    void bundledFallbacksCoverNavigationAndHelp();
    void themeProbeTracksTheActiveIconTheme();
    void bundledGlyphsCacheByColor();
    void buttonIconsFollowRuntimePaletteChanges();
    void darkModeIsFollowed();

    void helpOpensWithTheKeyboardAndReturns();
    void questionMarkOpensAndClosesHelp();
    void helpReturnsToTheDropTarget();
    void helpButtonMorphsAndToggles();
    void aboutPageShowsLicenceAndLinks();
    void aboutTextSurvivesAShortWindow();
    void spanishTranslationIsApplied();
    void englishCatalogResolvesEveryId();

private:
    QString m_savedThemeName;
    QString m_savedFallbackThemeName;
};

void TestEnquber::init()
{
    // Every case builds its window after this runs, so the English source
    // catalog has to be installed here (widgets read translations once, at
    // construction).
    QVERIFY(i18n::install(*qApp, QLocale(QLocale::English)));
}

void TestEnquber::initTestCase()
{
    // Several tests assert on the bundled fallback glyphs, which are only used
    // when no system icon theme answers. Point the theme lookup at a name that
    // cannot exist so those tests are deterministic even when the binary is run
    // from a desktop session instead of the offscreen platform.
    m_savedThemeName = QIcon::themeName();
    m_savedFallbackThemeName = QIcon::fallbackThemeName();
    QIcon::setThemeName(QStringLiteral("enquber-no-such-theme"));
    QIcon::setFallbackThemeName(QStringLiteral("enquber-no-such-theme"));
}

void TestEnquber::cleanupTestCase()
{
    QIcon::setThemeName(m_savedThemeName);
    QIcon::setFallbackThemeName(m_savedFallbackThemeName);
}

void TestEnquber::encodesText_data()
{
    QTest::addColumn<QString>("text");
    QTest::newRow("url") << QStringLiteral("https://example.com/some/path?a=1&b=2");
    QTest::newRow("plain") << QStringLiteral("hello world");
    QTest::newRow("unicode") << QStringLiteral("Grüße aus München ✓");
    QTest::newRow("multiline") << QStringLiteral("first line\nsecond line");
}

void TestEnquber::encodesText()
{
    QFETCH(QString, text);

    const qr::Code code = qr::Code::encode(text);

    QVERIFY(code.isValid());
    QVERIFY(code.error().isEmpty());
    QCOMPARE(code.text(), text);
    QVERIFY(code.modules() >= 21);
}

void TestEnquber::rejectsEmptyText()
{
    const qr::Code code = qr::Code::encode(QString());

    QVERIFY(!code.isValid());
    QVERIFY(!code.error().isEmpty());
    QVERIFY(code.toImage(4).isNull());
}

void TestEnquber::rejectsOversizedText()
{
    const qr::Code code = qr::Code::encode(QString(5000, QLatin1Char('a')));

    QVERIFY(!code.isValid());
    QVERIFY(!code.error().isEmpty());
}

void TestEnquber::rendersQuietZoneAndModules()
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

void TestEnquber::picksSmallestVersion()
{
    const qr::Code small = qr::Code::encode(QStringLiteral("hi"));
    const qr::Code large = qr::Code::encode(QString(1000, QLatin1Char('a')));

    QVERIFY(small.isValid());
    QVERIFY(large.isValid());
    QVERIFY(small.modules() < large.modules());
}

void TestEnquber::dropOnWindowShowsCode()
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

void TestEnquber::dropOnDropZoneShowsCode()
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

void TestEnquber::droppingUrlsPrefersTheUrl()
{
    auto *mime = new QMimeData;
    mime->setUrls({QUrl(QStringLiteral("https://url.example/x"))});
    mime->setText(QStringLiteral("https://text.example/y"));

    QCOMPARE(mime::textForQr(mime), QStringLiteral("https://url.example/x"));
    delete mime;
}

void TestEnquber::payloadSurvivesASourceThatStopsAnswering()
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

void TestEnquber::dropWithoutTextIsIgnored()
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

void TestEnquber::pasteEncodesClipboardText()
{
    MainWindow window;
    showAndActivate(&window);
    QCOMPARE(QApplication::activeWindow(), &window);
    QGuiApplication::clipboard()->setText(QStringLiteral("  https://paste.example  \n"));

    QTest::keyClick(&window, Qt::Key_V, Qt::ControlModifier);

    QVERIFY(window.hasCode());
    QCOMPARE(window.encodedText(), QStringLiteral("https://paste.example"));
}

void TestEnquber::pasteWithoutTextIsIgnored()
{
    MainWindow window;
    showAndActivate(&window);
    QGuiApplication::clipboard()->clear();

    QTest::keyClick(&window, Qt::Key_V, Qt::ControlModifier);

    QVERIFY(!window.hasCode());
}

void TestEnquber::copyPutsImageOnClipboard()
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

void TestEnquber::draggingOffersImageAndFile()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    // A real PNG on disk, which is what the payload builder is handed: a file
    // manager can only save a drop if the file it points at exists.
    const qr::Code code = qr::Code::encode(QStringLiteral("https://payload.example"));
    QVERIFY(code.isValid());
    const QImage image = code.toImage(4);
    const QString path = directory.filePath(QStringLiteral("payload.png"));
    QVERIFY(image.save(path, "PNG"));

    QScopedPointer<QMimeData> payload(mime::payloadForDrag(image, path));

    // Documents get the pixels, file managers get a file to copy.
    QVERIFY(payload->hasImage());
    QVERIFY(payload->hasFormat(QStringLiteral("image/png")));
    QVERIFY(payload->hasUrls());

    const QList<QUrl> urls = payload->urls();
    QCOMPARE(urls.size(), 1);
    QVERIFY(urls.first().isLocalFile());
    QCOMPARE(urls.first().toLocalFile(), path);

    QVERIFY(QFileInfo::exists(path));
    const QImage reloaded(path);
    QVERIFY(!reloaded.isNull());
    QCOMPARE(reloaded.size(), image.size());

    // The explicit PNG bytes are the same image as the file.
    const QImage fromBytes = QImage::fromData(payload->data(QStringLiteral("image/png")), "PNG");
    QVERIFY(!fromBytes.isNull());
    QCOMPARE(fromBytes.size(), image.size());
}

void TestEnquber::qrViewStartsDragOnGesture()
{
    MainWindow window;
    showAndActivate(&window);
    window.setText(QStringLiteral("https://drag.example"));

    auto *view = window.findChild<QrView *>();
    QVERIFY(view);
    QSignalSpy spy(view, &QrView::dragRequested);

    const QPoint start = view->rect().center();
    sendDragGesture(view, start, start + QPoint(QApplication::startDragDistance() + 5, 0));

    QCOMPARE(spy.count(), 1);
}

void TestEnquber::draggingBelowTheThresholdDoesNothing()
{
    MainWindow window;
    showAndActivate(&window);
    window.setText(QStringLiteral("https://drag.example"));

    auto *view = window.findChild<QrView *>();
    QVERIFY(view);
    QSignalSpy spy(view, &QrView::dragRequested);

    // A twitch of the hand is a click, not a drag.
    const QPoint start = view->rect().center();
    sendDragGesture(view, start, start + QPoint(2, 2));
    QCOMPARE(spy.count(), 0);

    // A move that carries no held button is not a drag either.
    sendDragGesture(view, start, start + QPoint(QApplication::startDragDistance() + 5, 0),
                    /*hold=*/false);
    QCOMPARE(spy.count(), 0);
}

void TestEnquber::draggingWithoutACodeDoesNothing()
{
    MainWindow window;
    showAndActivate(&window);
    QVERIFY(!window.hasCode());

    auto *view = window.findChild<QrView *>();
    QVERIFY(view);
    QSignalSpy spy(view, &QrView::dragRequested);

    const QPoint start = view->rect().center();
    sendDragGesture(view, start, start + QPoint(QApplication::startDragDistance() + 5, 0));
    QCOMPARE(spy.count(), 0);
}

void TestEnquber::longTextIsShapedForTheLabel()
{
    MainWindow window;
    showAndActivate(&window);
    auto *label = window.findChild<QLabel *>(QStringLiteral("encodedText"));
    QVERIFY(label);

    // A short link is shown as it is, with nothing to reveal.
    window.setText(QStringLiteral("https://example.com/short"));
    QCOMPARE(label->text(), QStringLiteral("https://example.com/short"));
    QVERIFY(label->toolTip().isEmpty());

    // Six short lines must not be handed to the label as six lines: the label
    // promises at most three, with the whole text behind a tooltip.
    window.setText(QStringLiteral("one\ntwo\nthree\nfour\nfive\nsix"));
    QCOMPARE(label->text().count(QLatin1Char('\n')), 2);
    QVERIFY2(label->text().endsWith(QChar(0x2026)), qPrintable(label->text()));
    QVERIFY(label->toolTip().contains(QStringLiteral("six")));

    // Nor may a link with no break opportunity become one enormous line.
    const QString unbreakable = QStringLiteral("https://example.com/") + QString(400, QLatin1Char('x'));
    window.setText(unbreakable);
    QVERIFY2(label->text().count(QLatin1Char('\n')) <= 2, qPrintable(label->text()));
    QVERIFY(label->text().endsWith(QChar(0x2026)));
    QVERIFY(label->toolTip().contains(QStringLiteral("xxxx")));
}

void TestEnquber::repeatingAStatusMessageRestartsItsTimeout()
{
    MainWindow window;
    showAndActivate(&window);
    window.setText(QStringLiteral("https://status.example"));

    auto *timer = window.findChild<QTimer *>(QStringLiteral("statusTimeout"));
    QVERIFY(timer);
    QVERIFY(timer->isSingleShot());

    window.copyToClipboard();
    QVERIFY(timer->isActive());
    QTest::qWait(800);

    // The timeout of the first message must not cut the second one short: the
    // second has to start the clock again, which shows up as the remaining
    // time jumping back up. Comparing two readings instead of absolute values
    // keeps this independent of how busy the machine is.
    const int before = timer->remainingTime();
    window.copyToClipboard();
    const int after = timer->remainingTime();
    QVERIFY2(after > before + 300,
             qPrintable(QStringLiteral("remaining %1 ms before, %2 ms after the second message")
                            .arg(before).arg(after)));
}

void TestEnquber::clearingReturnsToTheDropTarget_data()
{
    QTest::addColumn<int>("key");
    QTest::newRow("escape") << int(Qt::Key_Escape);
    QTest::newRow("backspace") << int(Qt::Key_Backspace);
    QTest::newRow("delete") << int(Qt::Key_Delete);
}

void TestEnquber::clearingReturnsToTheDropTarget()
{
    QFETCH(int, key);

    MainWindow window;
    showAndActivate(&window);
    window.setText(QStringLiteral("https://clear.example"));
    QVERIFY(window.hasCode());

    QTest::keyClick(&window, Qt::Key(key));

    QVERIFY(!window.hasCode());
    QVERIFY(window.encodedText().isEmpty());
    QCOMPARE(window.findChild<QStackedWidget *>()->currentIndex(), 0);
    QVERIFY(!window.findChild<QrView *>()->hasCode());
    auto *zone = window.findChild<DropZone *>();
    QVERIFY(zone->isVisible());

    // Nothing to export any more.
    QVERIFY(!window.saveTo(QDir::tempPath() + QStringLiteral("/should-not-exist.png")));
}

void TestEnquber::clearingWithTheButtonWorks()
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

void TestEnquber::droppingStillWorksAfterClearing()
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

void TestEnquber::clickingCopyButtonCopiesImage()
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

void TestEnquber::keyboardReachesTheButtons()
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

void TestEnquber::clickingSaveButtonWritesFile()
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

void TestEnquber::saveWritesPngFile()
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

void TestEnquber::saveAppendsPngSuffix()
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

void TestEnquber::quitShortcutClosesWindow()
{
    MainWindow window;
    showAndActivate(&window);
    QVERIFY(window.isVisible());

    // The quit action has to carry the platform's standard quit gesture and,
    // so that Ctrl+Q works where the theme leaves that key unbound, Ctrl+Q
    // itself. Finding it by text keeps the test off the action's name.
    QAction *quit = nullptr;
    const QList<QAction *> actions = window.findChildren<QAction *>();
    for (QAction *action : actions) {
        if (action->text().contains(QStringLiteral("quit"), Qt::CaseInsensitive)) {
            quit = action;
            break;
        }
    }
    QVERIFY(quit);
    const QList<QKeySequence> shortcuts = quit->shortcuts();
    QVERIFY(shortcuts.contains(QKeySequence(QStringLiteral("Ctrl+Q")))
            || shortcuts.contains(QKeySequence(QKeySequence::Quit)));

    QTest::keyClick(&window, Qt::Key_Q, Qt::ControlModifier);

    QVERIFY(!window.isVisible());
}

void TestEnquber::windowAcceptsDrops()
{
    MainWindow window;
    QVERIFY(window.acceptDrops());
    QVERIFY(window.findChild<DropZone *>()->acceptDrops());
    QVERIFY(window.minimumSizeHint().isValid());
}

void TestEnquber::buttonsCarryIcons()
{
    // The offscreen platform always reports a null QIcon::fromTheme, so a
    // non-null icon here can only come from the bundled fallback glyphs. That
    // is exactly the plain-XDG/hicolor case the fallbacks exist for.
    MainWindow window;
    showAndActivate(&window);
    window.setText(QStringLiteral("https://icons.example"));

    for (const QString &needle : {QStringLiteral("copy"), QStringLiteral("save"), QStringLiteral("lear")}) {
        QPushButton *button = buttonContaining(&window, needle);
        QVERIFY2(button, qPrintable(needle));
        QVERIFY2(!button->icon().isNull(), qPrintable(needle));
        QVERIFY2(!button->icon().pixmap(24).isNull(), qPrintable(needle));
    }
}

void TestEnquber::dropZoneCarriesAnIcon()
{
    MainWindow window;
    showAndActivate(&window);
    auto *zone = window.findChild<DropZone *>();
    QVERIFY(zone);

    // The drop target shows its link glyph through a QLabel pixmap.
    bool hasPixmap = false;
    const QList<QLabel *> labels = zone->findChildren<QLabel *>();
    for (QLabel *label : labels) {
        if (!label->pixmap(Qt::ReturnByValue).isNull()) {
            hasPixmap = true;
            break;
        }
    }
    QVERIFY(hasPixmap);
}

void TestEnquber::windowUsesTheBundledAppIcon()
{
    MainWindow window;
    showAndActivate(&window);

    const QImage actual = window.windowIcon().pixmap(64).toImage();
    QVERIFY(!actual.isNull());

    const QImage expected = QIcon(QStringLiteral(":/enquber/icons/enquber.png")).pixmap(64).toImage();
    QVERIFY(!expected.isNull());
    QCOMPARE(actual, expected);
}

void TestEnquber::fallbackIconsFollowThePalette()
{
    const QPalette original = QApplication::palette();

    // On a dark palette the glyph has to come out light...
    QPalette dark = original;
    dark.setColor(QPalette::WindowText, QColor(Qt::white));
    QApplication::setPalette(dark);
    const QColor onDark = inkOf(theme::icon({"edit-copy"}).pixmap(64).toImage());

    // ...and on a light one, dark.
    QPalette light = original;
    light.setColor(QPalette::WindowText, QColor(Qt::black));
    QApplication::setPalette(light);
    const QColor onLight = inkOf(theme::icon({"edit-copy"}).pixmap(64).toImage());

    QApplication::setPalette(original);

    QVERIFY(onDark.isValid());
    QVERIFY(onLight.isValid());
    QVERIFY2(onDark.lightness() > 200, qPrintable(onDark.name()));
    QVERIFY2(onLight.lightness() < 80, qPrintable(onLight.name()));
}

void TestEnquber::bundledFallbacksCoverNavigationAndHelp()
{
    // The offscreen platform never answers from the icon theme, so a non-null
    // icon here can only be a bundled glyph. Both the freedesktop names a
    // toolbar would use and the Feather names themselves have to resolve.
    for (const char *name : {"go-previous", "help-contents", "help-about", "dialog-information",
                             "arrow-left", "help-circle", "info"}) {
        QVERIFY2(!theme::icon({name}).isNull(), name);
    }

    // A name with no bundled glyph stays null instead of guessing.
    QVERIFY(theme::icon({"this-icon-does-not-exist"}).isNull());
}

void TestEnquber::themeProbeTracksTheActiveIconTheme()
{
    // Two throwaway themes: one answers the probe ("edit-copy"), the other does
    // not but still carries another name the window asks for ("document-save").
    // Their icons are magenta and cyan, colors no palette-tinted bundled glyph
    // can take, so the theme and fallback branches are told apart unambiguously.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString magentaName = QStringLiteral("enquber-probe-magenta");
    const QString cyanName = QStringLiteral("enquber-probe-cyan");
    QVERIFY(writeIconTheme(root.path(), magentaName, QStringLiteral("edit-copy"),
                           QColor(255, 0, 255)));
    QVERIFY(writeIconTheme(root.path(), cyanName, QStringLiteral("document-save"),
                           QColor(0, 255, 255)));

    // What the bundled save glyph looks like while the active theme answers
    // nothing at all. The cyan theme must not be able to replace it: it only
    // carries document-save, not the probe, so the lookup has to go negative.
    const QImage bundledSave =
        theme::icon({"document-save", "document-save-as"}).pixmap(16).toImage();
    QVERIFY(!bundledSave.isNull());

    ThemeStateRestorer restore;
    QIcon::setThemeSearchPaths({root.path()});
    QIcon::setFallbackThemeName(QString());

    // A theme that answers the probe hands back its own icon.
    QIcon::setThemeName(magentaName);
    const QColor magenta = inkOf(theme::icon({"edit-copy"}).pixmap(16).toImage());
    QVERIFY2(magenta.isValid(), "the magenta theme did not provide edit-copy");
    QCOMPARE(magenta, QColor(255, 0, 255));

    // A theme without the probe re-probes and goes negative, so the whole lookup
    // falls back to the bundled glyph instead of the theme's cyan document-save.
    // A cache that ignored the theme name would keep the magenta answer and hand
    // back the cyan icon here.
    QIcon::setThemeName(cyanName);
    QCOMPARE(theme::icon({"document-save", "document-save-as"}).pixmap(16).toImage(),
             bundledSave);

    // Switching back has to re-probe positive again and return the theme icon; a
    // cache stuck on the cyan answer would keep the bundled glyph.
    QIcon::setThemeName(magentaName);
    QCOMPARE(inkOf(theme::icon({"edit-copy"}).pixmap(16).toImage()), QColor(255, 0, 255));
}

void TestEnquber::bundledGlyphsCacheByColor()
{
    // The bundled-glyph cache is keyed on the tint, so the same glyph under two
    // foreground colors must not collide, and the first color has to come back
    // exactly from its still-cached entry.
    const QPalette original = QApplication::palette();

    QPalette light = original;
    light.setColor(QPalette::WindowText, QColor(Qt::black));
    QApplication::setPalette(light);
    const QImage onLight = theme::icon({"edit-copy"}).pixmap(64).toImage();

    QPalette dark = original;
    dark.setColor(QPalette::WindowText, QColor(Qt::white));
    QApplication::setPalette(dark);
    const QImage onDark = theme::icon({"edit-copy"}).pixmap(64).toImage();

    QApplication::setPalette(light);
    const QImage onLightAgain = theme::icon({"edit-copy"}).pixmap(64).toImage();

    QApplication::setPalette(original);

    QVERIFY(!onLight.isNull());
    QVERIFY(!onDark.isNull());
    QVERIFY2(onLight != onDark, "the cache returned one color's glyph for another");
    QCOMPARE(onLightAgain, onLight);
}

void TestEnquber::buttonIconsFollowRuntimePaletteChanges()
{
    // The fallback glyph is tinted when the icon is built, so a live palette
    // change has to rebuild the button icons or they keep the old color.
    MainWindow window;
    showAndActivate(&window);
    window.setText(QStringLiteral("https://palette.example"));
    QPushButton *copy = buttonContaining(&window, QStringLiteral("copy"));
    QVERIFY(copy);
    QVERIFY(!copy->icon().isNull());

    const QPalette original = QApplication::palette();

    QPalette dark = original;
    dark.setColor(QPalette::WindowText, QColor(Qt::white));
    QApplication::setPalette(dark);
    QCoreApplication::processEvents();
    const QColor onDark = inkOf(copy->icon().pixmap(64).toImage());

    QPalette light = original;
    light.setColor(QPalette::WindowText, QColor(Qt::black));
    QApplication::setPalette(light);
    QCoreApplication::processEvents();
    const QColor onLight = inkOf(copy->icon().pixmap(64).toImage());

    QApplication::setPalette(original);

    QVERIFY(onDark.isValid());
    QVERIFY(onLight.isValid());
    QVERIFY2(onDark.lightness() > 200, qPrintable(onDark.name()));
    QVERIFY2(onLight.lightness() < 80, qPrintable(onLight.name()));
}

void TestEnquber::darkModeIsFollowed()
{
    // The desktop hands the theme to the app as a palette change: Qt updates
    // the default palette when the system color scheme flips. Everything the
    // widgets draw themselves has to follow that, and the dimmed labels in
    // particular must not freeze the placeholder color they were built with.
    const QPalette original = QApplication::palette();

    const QColor lightPlaceholder(0x76, 0x76, 0x76);
    const QColor darkPlaceholder(0xc0, 0xc0, 0xc0);
    QPalette light = original;
    light.setColor(QPalette::PlaceholderText, lightPlaceholder);
    QPalette dark = original;
    dark.setColor(QPalette::Window, QColor(24, 24, 24));
    dark.setColor(QPalette::Base, QColor(18, 18, 18));
    dark.setColor(QPalette::WindowText, QColor(240, 240, 240));
    dark.setColor(QPalette::PlaceholderText, darkPlaceholder);

    QApplication::setPalette(light);
    QCoreApplication::processEvents();

    MainWindow window;
    showAndActivate(&window);

    auto *dropHint = window.findChild<QLabel *>(QStringLiteral("dropZoneHint"));
    QVERIFY(dropHint);
    QVERIFY(dropHint->isVisible());
    QCOMPARE(int(dropHint->foregroundRole()), int(QPalette::PlaceholderText));
    QCOMPARE(drawnTextColor(dropHint), lightPlaceholder);

    QApplication::setPalette(dark);
    QCoreApplication::processEvents();

    QCOMPARE(drawnTextColor(dropHint), darkPlaceholder);

    // The about page mutes its labels through its own helper.
    QTest::keyClick(&window, Qt::Key_H, Qt::ControlModifier);
    auto *aboutHint = window.findChild<QLabel *>(QStringLiteral("aboutHint"));
    QVERIFY(aboutHint);
    QVERIFY(aboutHint->isVisible());
    QCOMPARE(int(aboutHint->foregroundRole()), int(QPalette::PlaceholderText));
    QCOMPARE(drawnTextColor(aboutHint), darkPlaceholder);

    // And so does the status line, which only appears after an action.
    QTest::keyClick(&window, Qt::Key_Escape);
    window.setText(QStringLiteral("https://dark-mode.example"));
    window.copyToClipboard();
    auto *status = window.findChild<QLabel *>(QStringLiteral("statusLabel"));
    QVERIFY(status);
    QVERIFY(status->isVisible());
    QCOMPARE(int(status->foregroundRole()), int(QPalette::PlaceholderText));
    QCOMPARE(drawnTextColor(status), darkPlaceholder);

    // Switching back has to bring the light colors back, not leave the dark
    // ones behind.
    QApplication::setPalette(light);
    QCoreApplication::processEvents();
    QCOMPARE(drawnTextColor(status), lightPlaceholder);

    QTest::keyClick(&window, Qt::Key_Escape);
    QVERIFY(dropHint->isVisible());
    QCOMPARE(drawnTextColor(dropHint), lightPlaceholder);

    QApplication::setPalette(original);
}

void TestEnquber::helpOpensWithTheKeyboardAndReturns()
{
    MainWindow window;
    showAndActivate(&window);
    window.setText(QStringLiteral("https://help.example"));

    auto *stack = window.findChild<QStackedWidget *>();
    QCOMPARE(stack->currentIndex(), 1);

    QTest::keyClick(&window, Qt::Key_H, Qt::ControlModifier);
    QCOMPARE(stack->currentIndex(), 2);
    // The code stays put; Escape must leave the page, not clear the code.
    QVERIFY(window.hasCode());

    QTest::keyClick(&window, Qt::Key_Escape);
    QCOMPARE(stack->currentIndex(), 1);
    QCOMPARE(window.encodedText(), QStringLiteral("https://help.example"));
}

void TestEnquber::questionMarkOpensAndClosesHelp()
{
    MainWindow window;
    showAndActivate(&window);
    auto *stack = window.findChild<QStackedWidget *>();
    QCOMPARE(stack->currentIndex(), 0);

    QTest::keyClick(&window, Qt::Key_Question);
    QCOMPARE(stack->currentIndex(), 2);

    // The shortcut toggles, so the same key takes you back.
    QTest::keyClick(&window, Qt::Key_Question);
    QCOMPARE(stack->currentIndex(), 0);
}

void TestEnquber::helpReturnsToTheDropTarget()
{
    MainWindow window;
    showAndActivate(&window);
    auto *stack = window.findChild<QStackedWidget *>();
    QVERIFY(!window.hasCode());

    QTest::keyClick(&window, Qt::Key_H, Qt::ControlModifier);
    QCOMPARE(stack->currentIndex(), 2);

    QTest::keyClick(&window, Qt::Key_Escape);
    QCOMPARE(stack->currentIndex(), 0);
    QVERIFY(window.findChild<DropZone *>()->isVisible());
}

void TestEnquber::helpButtonMorphsAndToggles()
{
    MainWindow window;
    showAndActivate(&window);

    auto *stack = window.findChild<QStackedWidget *>();
    auto *button = window.findChild<QAbstractButton *>(QStringLiteral("helpButton"));
    QVERIFY(button);
    QVERIFY(button->isVisible());
    QCOMPARE(button->accessibleName(), QStringLiteral("Help and info"));

    QTest::mouseClick(button, Qt::LeftButton);
    QCOMPARE(stack->currentIndex(), 2);
    // On the info page the button stops asking and points back.
    QCOMPARE(button->accessibleName(), QStringLiteral("Back to Enquber"));

    QTest::mouseClick(button, Qt::LeftButton);
    QCOMPARE(stack->currentIndex(), 0);
    QCOMPARE(button->accessibleName(), QStringLiteral("Help and info"));
}

void TestEnquber::aboutPageShowsLicenceAndLinks()
{
    MainWindow window;
    showAndActivate(&window);

    auto *copyright = window.findChild<QLabel *>(QStringLiteral("aboutCopyright"));
    QVERIFY(copyright);
    QVERIFY(copyright->text().contains(QStringLiteral("Morgan Astra")));

    auto *licence = window.findChild<QLabel *>(QStringLiteral("aboutLicence"));
    QVERIFY(licence);
    QVERIFY(licence->text().contains(QStringLiteral("GNU General Public License")));

    auto *links = window.findChild<QLabel *>(QStringLiteral("aboutLinks"));
    QVERIFY(links);
    QVERIFY(links->openExternalLinks());
    QVERIFY(links->text().contains(QStringLiteral("github.com/morganastra/enquber")));
    QVERIFY(links->text().contains(QStringLiteral("qt.io")));
    QVERIFY(links->text().contains(QStringLiteral("libqrencode")));
    // The bundled fallback glyphs are Feather Icons, credited under MIT.
    QVERIFY(links->text().contains(QStringLiteral("Feather Icons")));
    QVERIFY(links->text().contains(QStringLiteral("Cole Bemis")));
    QVERIFY(links->text().contains(QStringLiteral("MIT")));
}

void TestEnquber::aboutTextSurvivesAShortWindow()
{
    MainWindow window;
    window.resize(640, 420);
    showAndActivate(&window);
    QTest::keyClick(&window, Qt::Key_H, Qt::ControlModifier);

    auto *licence = window.findChild<QLabel *>(QStringLiteral("aboutLicence"));
    QVERIFY(licence);
    QVERIFY(licence->isVisible());

    // A short window used to squash the wrapped licence to a single clipped
    // line; it now keeps the height its text needs at its fixed width.
    const int needed = licence->heightForWidth(licence->width());
    QVERIFY(needed > 0);
    QVERIFY2(licence->height() >= needed,
             qPrintable(QStringLiteral("licence is %1 px, needs %2")
                            .arg(licence->height()).arg(needed)));
}

void TestEnquber::spanishTranslationIsApplied()
{
    // Proves the ID-based catalogs really localize: a Spanish window must be
    // built from Spanish strings, not from English falling through.
    QVERIFY(i18n::install(*qApp, QLocale(QStringLiteral("es"))));

    QCOMPARE(qtTrId("dropzone.title"), QStringLiteral("Suelta un enlace aquí"));
    QCOMPARE(qtTrId("mainwindow.action.copy"), QStringLiteral("&Copiar imagen"));
    QCOMPARE(qtTrId("mainwindow.status.saved-to"), QStringLiteral("Guardado en %1"));

    MainWindow window;
    showAndActivate(&window);

    // The widgets themselves, not only qtTrId(), carry the translation.
    auto *zone = window.findChild<DropZone *>();
    QVERIFY(zone);
    bool sawSpanishTitle = false;
    for (QLabel *label : zone->findChildren<QLabel *>()) {
        sawSpanishTitle = sawSpanishTitle || label->text() == QStringLiteral("Suelta un enlace aquí");
    }
    QVERIFY(sawSpanishTitle);

    QPushButton *copy = buttonContaining(&window, QStringLiteral("Copiar"));
    QVERIFY(copy);

    QTest::keyClick(&window, Qt::Key_H, Qt::ControlModifier);
    auto *hint = window.findChild<QLabel *>(QStringLiteral("aboutHint"));
    QVERIFY(hint);
    QCOMPARE(hint->text(), QStringLiteral("Pulsa Esc para volver"));

    // Switching back must restore English; init() relies on the same call for
    // every other test.
    QVERIFY(i18n::install(*qApp, QLocale(QLocale::English)));
    QCOMPARE(qtTrId("dropzone.title"), QStringLiteral("Drop a link here"));
}

void TestEnquber::englishCatalogResolvesEveryId()
{
    // Every ID the app uses has to resolve to English, never to the id itself.
    // This guards against a catalog that was not regenerated after a source
    // change, and against an lrelease that dropped source-language entries.
    const QStringList ids = {
        QStringLiteral("app.name"),
        QStringLiteral("about.version"),
        QStringLiteral("about.tagline"),
        QStringLiteral("about.copyright"),
        QStringLiteral("about.licence"),
        QStringLiteral("about.links"),
        QStringLiteral("about.hint"),
        QStringLiteral("dropzone.title"),
        QStringLiteral("dropzone.hint"),
        QStringLiteral("mainwindow.help.tooltip"),
        QStringLiteral("mainwindow.help.accessible"),
        QStringLiteral("mainwindow.action.paste"),
        QStringLiteral("mainwindow.action.copy"),
        QStringLiteral("mainwindow.action.copy.tooltip"),
        QStringLiteral("mainwindow.action.save"),
        QStringLiteral("mainwindow.action.save.tooltip"),
        QStringLiteral("mainwindow.action.clear"),
        QStringLiteral("mainwindow.action.clear.tooltip"),
        QStringLiteral("mainwindow.action.help"),
        QStringLiteral("mainwindow.action.quit"),
        QStringLiteral("mainwindow.action.quit.tooltip"),
        QStringLiteral("mainwindow.status.clipboard-empty"),
        QStringLiteral("mainwindow.help.back.tooltip"),
        QStringLiteral("mainwindow.help.back.accessible"),
        QStringLiteral("mainwindow.status.copied"),
        QStringLiteral("mainwindow.status.drag-failed"),
        QStringLiteral("mainwindow.dialog.save.title"),
        QStringLiteral("mainwindow.dialog.save.filter"),
        QStringLiteral("mainwindow.status.write-failed"),
        QStringLiteral("mainwindow.status.saved-to"),
        QStringLiteral("mainwindow.status.drop-replace"),
        QStringLiteral("qrcode.error.too-much-data"),
        QStringLiteral("qrcode.error.out-of-memory"),
        QStringLiteral("qrcode.error.nothing"),
    };

    for (const QString &id : ids) {
        const QString text = qtTrId(id.toUtf8().constData());
        QVERIFY2(!text.isEmpty(), qPrintable(id));
        QVERIFY2(text != id, qPrintable(id));
    }
}

QTEST_MAIN(TestEnquber)
#include "tst_enquber.moc"
