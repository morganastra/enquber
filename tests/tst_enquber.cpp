#include "dropzone.h"
#include "i18n.h"
#include "mainwindow.h"
#include "mimeimage.h"
#include "mimetext.h"
#include "qrcode.h"
#include "qrview.h"
#include "theme.h"
#include "typeeditor.h"

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
#include <QTextBlock>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
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

/// Ends a drag that sendDragEnter started, like Qt does when the cursor leaves
/// the target.
void sendDragLeave(QWidget *target)
{
    QDragLeaveEvent event;
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

/// Puts the application palette back when it goes out of scope, for the same
/// reason as ThemeStateRestorer. Construct it before the first setPalette()
/// call, or its destructor would restore the synthetic palette instead of the
/// original.
class PaletteStateRestorer
{
public:
    PaletteStateRestorer() : m_palette(QApplication::palette()) {}

    ~PaletteStateRestorer() { QApplication::setPalette(m_palette); }

    PaletteStateRestorer(const PaletteStateRestorer &) = delete;
    PaletteStateRestorer &operator=(const PaletteStateRestorer &) = delete;

private:
    QPalette m_palette;
};

/// A QrView whose device pixel ratio can be changed while it lives, to prove
/// the cache notices a DPR change even when the module size stays the same.
/// QPaintDevice::devicePixelRatio() reads the scaled metric on Qt 6.5-6.7, and
/// on 6.8+ for exactly 1x or 2x, so overriding it covers the 1<->2 regression
/// on every supported Qt. The encoded pair (Qt 6.8+) keeps the probe correct
/// for fractional ratios too.
class DprProbeView : public QrView
{
public:
    qreal probeDpr = 1.0;

    int metric(QPaintDevice::PaintDeviceMetric m) const override
    {
        switch (m) {
        case QPaintDevice::PdmDevicePixelRatioScaled:
            return static_cast<int>(probeDpr * QPaintDevice::devicePixelRatioFScale());
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        case QPaintDevice::PdmDevicePixelRatioF_EncodedA:
        case QPaintDevice::PdmDevicePixelRatioF_EncodedB:
            return QPaintDevice::encodeMetricF(m, probeDpr);
#endif
        default:
            return QrView::metric(m);
        }
    }
};

/// Bounding box, in device pixels, of the pure-white pixels of @p painted: on a
/// gray ground the symbol's quiet zone is the only pure white, so the box is
/// the painted symbol.
QRect whiteBounds(const QImage &painted)
{
    int left = painted.width();
    int top = painted.height();
    int right = -1;
    int bottom = -1;
    for (int y = 0; y < painted.height(); ++y) {
        for (int x = 0; x < painted.width(); ++x) {
            if (painted.pixel(x, y) == qRgb(255, 255, 255)) {
                left = qMin(left, x);
                top = qMin(top, y);
                right = qMax(right, x);
                bottom = qMax(bottom, y);
            }
        }
    }
    return right >= 0 ? QRect(QPoint(left, top), QPoint(right, bottom)) : QRect();
}

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
    void dragLeaveResetsTheHighlightAndForgetsTheText();
    void refusedDragsKeepAnUnrelatedStatus();
    void dropReplaceStatusIsClearedOnLeave();

    void pasteEncodesClipboardText();
    void pasteWithoutTextIsIgnored();

    void typeEditorReturnSubmits();
    void typeEditorModifiedReturnInsertsANewline();
    void typeEditorEscapeIsReportedNotTyped();
    void typeEditorTabMovesOnInsteadOfTyping();
    void typeEditorKeepsEveryParagraphCentered();

    void liveTypeOpensTheFieldWithPlaceholder();
    void liveTypeEncodesAsYouGo();
    void liveClearingRestoresThePlaceholder();
    void livePrefillsTheCurrentCode();
    void liveCtrlEnterInsertsANewline();
    void liveEditorAcceptsPaste();
    void liveReturnClosesTheField();
    void liveEscapeRestoresThePreviousCode();
    void dropZoneClickOpensTheTypeField();
    void captionEditorMatchesTheSymbolWidth();
    void captionEditorTextIsCentered();

    void copyPutsImageOnClipboard();
    void draggingOffersImageAndFile();
    void qrViewStartsDragOnGesture();
    void draggingBelowTheThresholdDoesNothing();
    void draggingWithoutACodeDoesNothing();
    void codeSideIsZeroWithoutACode();
    void codeSideIsTheSizeOfThePaintedSymbol_data();
    void codeSideIsTheSizeOfThePaintedSymbol();
    void dprChangeRebuildsTheCache();
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
    void saveReportsAWriteFailure();
    void exportGeometry_data();
    void exportGeometry();
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
    void aboutPageShowsLicenseAndLinks();
    void aboutTextSurvivesAShortWindow();
    void resizeWhileHelpIsUpReshapesTheCaption();
    void spanishTranslationIsApplied();
    void englishCatalogResolvesEveryId();

    void dropWhileTypingReplacesTheEdit();
    void helpWhileTypingCancelsTheEditAndComesBack();
    void committingAnEmptyEditGoesBackToTheDropTarget();
    void escapeWithNoPreviousCodeReturnsToTheDropTarget();

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

    // A module size below one pixel has nothing to render.
    QVERIFY(code.toImage(0).isNull());
    QVERIFY(code.toImage(-1).isNull());

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

    // The zone is a drop indicator, not a target of its own: a drag over it
    // is routed to the window, which lights the zone up and encodes the text.
    QScopedPointer<QMimeData> mime(textMime(QStringLiteral("https://zone.example")));
    sendDragEnter(&window, mime.data());
    QVERIFY(zone->isActive());
    performDrop(&window, mime.data());

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
    mime::ObservedText observed;
    QCOMPARE(observed.observe(hovering.data()), QStringLiteral("https://slow-source.example"));

    auto *mute = new QMimeData; // formats advertised, but no data behind them
    mute->setData(QStringLiteral("text/plain"), QByteArray());
    QVERIFY(mute->hasFormat(QStringLiteral("text/plain")));
    QCOMPARE(observed.resolve(mute), QStringLiteral("https://slow-source.example"));

    // A payload that does answer wins over the observation.
    QScopedPointer<QMimeData> answering(textMime(QStringLiteral("https://fast-source.example")));
    QCOMPARE(observed.resolve(answering.data()), QStringLiteral("https://fast-source.example"));

    // And once the drag is over the observation is gone.
    observed.forget();
    QVERIFY(observed.resolve(mute).isEmpty());
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

void TestEnquber::dragLeaveResetsTheHighlightAndForgetsTheText()
{
    MainWindow window;
    showAndActivate(&window);
    auto *zone = window.findChild<DropZone *>();
    QVERIFY(zone);

    QScopedPointer<QMimeData> hovering(textMime(QStringLiteral("https://leave.example")));
    sendDragEnter(&window, hovering.data());
    QVERIFY(zone->isActive());

    sendDragLeave(&window);
    QVERIFY(!zone->isActive());

    // The leave forgot what was read while hovering, so a source that stops
    // answering after it has nothing left to hand over at the drop.
    auto *mute = new QMimeData;
    mute->setData(QStringLiteral("text/plain"), QByteArray());
    performDrop(&window, mute);
    delete mute;

    QVERIFY(!window.hasCode());
}

void TestEnquber::refusedDragsKeepAnUnrelatedStatus()
{
    MainWindow window;
    showAndActivate(&window);
    auto *zone = window.findChild<DropZone *>();
    QVERIFY(zone);

    // A status from an unrelated action, still on screen.
    QGuiApplication::clipboard()->clear();
    window.pasteFromClipboard();
    auto *status = window.findChild<QLabel *>(QStringLiteral("statusLabel"));
    QVERIFY(status);
    QVERIFY(status->isVisible());

    // A drag the window cannot encode is refused without a message of its own,
    // so it must not light the zone up either.
    auto *mime = new QMimeData;
    mime->setData(QStringLiteral("image/png"), QByteArray("not really a png"));

    sendDragEnter(&window, mime);
    QVERIFY(!zone->isActive());
    QVERIFY(status->isVisible());

    sendDragLeave(&window);
    QVERIFY(status->isVisible());

    performDrop(&window, mime);
    QVERIFY(status->isVisible());
    QVERIFY(!window.hasCode());

    delete mime;
}

void TestEnquber::dropReplaceStatusIsClearedOnLeave()
{
    MainWindow window;
    showAndActivate(&window);
    window.setText(QStringLiteral("https://replace.example"));
    auto *status = window.findChild<QLabel *>(QStringLiteral("statusLabel"));
    QVERIFY(status);
    QVERIFY(!status->isVisible());

    // With a code on screen the drag promises a replacement; leaving takes
    // that promise back.
    QScopedPointer<QMimeData> mime(textMime(QStringLiteral("https://new.example")));
    sendDragEnter(&window, mime.data());
    QVERIFY(status->isVisible());
    sendDragLeave(&window);
    QVERIFY(!status->isVisible());

    // The drag's ownership of the status ended with it: a later refused drag
    // must not clear a message it did not show.
    QGuiApplication::clipboard()->clear();
    window.pasteFromClipboard();
    QVERIFY(status->isVisible());

    auto *refused = new QMimeData;
    refused->setData(QStringLiteral("image/png"), QByteArray("not really a png"));
    sendDragEnter(&window, refused);
    sendDragLeave(&window);
    QVERIFY(status->isVisible());
    delete refused;
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

void TestEnquber::typeEditorReturnSubmits()
{
    TypeEditor editor;
    QSignalSpy submitted(&editor, &TypeEditor::submitted);
    QSignalSpy cancelled(&editor, &TypeEditor::cancelled);

    QTest::keyClicks(&editor, QStringLiteral("abc"));
    QTest::keyClick(&editor, Qt::Key_Return);
    QTest::keyClick(&editor, Qt::Key_Enter);

    // Return, on the main keys or the keypad, finishes the edit and is not
    // typed into the field.
    QCOMPARE(submitted.count(), 2);
    QCOMPARE(cancelled.count(), 0);
    QCOMPARE(editor.toPlainText(), QStringLiteral("abc"));
}

void TestEnquber::typeEditorModifiedReturnInsertsANewline()
{
    TypeEditor editor;
    QSignalSpy submitted(&editor, &TypeEditor::submitted);

    QTest::keyClicks(&editor, QStringLiteral("a"));
    QTest::keyClick(&editor, Qt::Key_Return, Qt::ControlModifier);
    QTest::keyClicks(&editor, QStringLiteral("b"));
    QTest::keyClick(&editor, Qt::Key_Return, Qt::ShiftModifier);
    QTest::keyClicks(&editor, QStringLiteral("c"));

    QCOMPARE(editor.toPlainText(), QStringLiteral("a\nb\nc"));
    QCOMPARE(submitted.count(), 0);
}

void TestEnquber::typeEditorEscapeIsReportedNotTyped()
{
    TypeEditor editor;
    QSignalSpy cancelled(&editor, &TypeEditor::cancelled);

    QTest::keyClicks(&editor, QStringLiteral("abc"));
    QTest::keyClick(&editor, Qt::Key_Escape);

    QCOMPARE(cancelled.count(), 1);
    QCOMPARE(editor.toPlainText(), QStringLiteral("abc"));
}

void TestEnquber::typeEditorTabMovesOnInsteadOfTyping()
{
    QWidget window;
    auto *layout = new QVBoxLayout(&window);
    auto *editor = new TypeEditor(&window);
    auto *next = new QPushButton(&window);
    layout->addWidget(editor);
    layout->addWidget(next);
    showAndActivate(&window);
    editor->setFocus(Qt::OtherFocusReason);
    QCOMPARE(QApplication::focusWidget(), editor);

    // Tab moves focus on instead of inserting a tab.
    QTest::keyClicks(editor, QStringLiteral("a"));
    QTest::keyClick(editor, Qt::Key_Tab);

    QCOMPARE(editor->toPlainText(), QStringLiteral("a"));
    QCOMPARE(QApplication::focusWidget(), next);
}

void TestEnquber::typeEditorKeepsEveryParagraphCentered()
{
    TypeEditor editor;
    const auto allCentered = [&editor] {
        for (QTextBlock block = editor.document()->begin(); block.isValid(); block = block.next()) {
            if (block.blockFormat().alignment() != Qt::AlignHCenter) {
                return false;
            }
        }
        return true;
    };

    // Alignment lives in the block format, which setPlainText() and clear()
    // reset, so each of these has to be put right again.
    QVERIFY(allCentered());

    editor.setPlainText(QStringLiteral("one\ntwo"));
    QVERIFY(allCentered());

    editor.moveCursor(QTextCursor::End);
    QTest::keyClick(&editor, Qt::Key_Return, Qt::ControlModifier);
    QTest::keyClicks(&editor, QStringLiteral("three"));
    QCOMPARE(editor.toPlainText(), QStringLiteral("one\ntwo\nthree"));
    QVERIFY(allCentered());

    editor.clear();
    QVERIFY(allCentered());
}

void TestEnquber::liveTypeOpensTheFieldWithPlaceholder()
{
    MainWindow window;
    showAndActivate(&window);

    QTest::keyClick(&window, Qt::Key_L, Qt::ControlModifier);

    auto *editor = window.findChild<TypeEditor *>(QStringLiteral("captionEditor"));
    QVERIFY(editor);
    QVERIFY(editor->isVisible());
    // Typing has to land in the field, not be swallowed by a shortcut.
    QCOMPARE(QApplication::focusWidget(), editor);
    // The field starts empty, but the page is not blank: a placeholder symbol
    // stands in. It is not the user's code, so hasCode() stays false.
    QVERIFY(!window.hasCode());
    QVERIFY(window.findChild<QrView *>()->hasCode());
    QCOMPARE(window.findChild<QStackedWidget *>()->currentIndex(), 1);
    // Nothing real to export yet, so the buttons must not look live.
    QPushButton *copy = buttonContaining(&window, QStringLiteral("copy"));
    QVERIFY(copy);
    QVERIFY(!copy->isEnabled());
}

void TestEnquber::liveTypeEncodesAsYouGo()
{
    MainWindow window;
    showAndActivate(&window);

    QTest::keyClick(&window, Qt::Key_L, Qt::ControlModifier);
    auto *editor = window.findChild<TypeEditor *>(QStringLiteral("captionEditor"));
    QVERIFY(editor);

    QTest::keyClicks(editor, QStringLiteral("live"));
    // No Return: the symbol is already built from what has been typed.
    QVERIFY(window.hasCode());
    QCOMPARE(window.encodedText(), QStringLiteral("live"));
}

void TestEnquber::liveClearingRestoresThePlaceholder()
{
    MainWindow window;
    showAndActivate(&window);

    QTest::keyClick(&window, Qt::Key_L, Qt::ControlModifier);
    auto *editor = window.findChild<TypeEditor *>(QStringLiteral("captionEditor"));
    QVERIFY(editor);
    QTest::keyClicks(editor, QStringLiteral("gone"));
    QVERIFY(window.hasCode());

    editor->clear();

    // Back to empty: the placeholder returns, the user's code is gone, and the
    // field stays open.
    QVERIFY(!window.hasCode());
    QVERIFY(window.findChild<QrView *>()->hasCode());
    QVERIFY(editor->isVisible());
}

void TestEnquber::livePrefillsTheCurrentCode()
{
    MainWindow window;
    showAndActivate(&window);
    window.setText(QStringLiteral("https://before.example"));

    QTest::keyClick(&window, Qt::Key_L, Qt::ControlModifier);
    auto *editor = window.findChild<TypeEditor *>(QStringLiteral("captionEditor"));
    QVERIFY(editor);
    // Reopening the field starts from what is currently encoded.
    QCOMPARE(editor->toPlainText(), QStringLiteral("https://before.example"));

    editor->setPlainText(QStringLiteral("https://typed.example"));
    QTest::keyClick(editor, Qt::Key_Return);

    QVERIFY(window.hasCode());
    QCOMPARE(window.encodedText(), QStringLiteral("https://typed.example"));
    QVERIFY(!editor->isVisible());
}

void TestEnquber::liveCtrlEnterInsertsANewline()
{
    MainWindow window;
    showAndActivate(&window);

    QTest::keyClick(&window, Qt::Key_L, Qt::ControlModifier);
    auto *editor = window.findChild<TypeEditor *>(QStringLiteral("captionEditor"));
    QVERIFY(editor);

    QTest::keyClicks(editor, QStringLiteral("one"));
    QTest::keyClick(editor, Qt::Key_Return, Qt::ControlModifier);
    QTest::keyClicks(editor, QStringLiteral("two"));

    QCOMPARE(editor->toPlainText(), QStringLiteral("one\ntwo"));
    QCOMPARE(window.encodedText(), QStringLiteral("one\ntwo"));
}

void TestEnquber::liveEditorAcceptsPaste()
{
    MainWindow window;
    showAndActivate(&window);
    QGuiApplication::clipboard()->setText(QStringLiteral("pasted\nvalue"));

    QTest::keyClick(&window, Qt::Key_L, Qt::ControlModifier);
    auto *editor = window.findChild<TypeEditor *>(QStringLiteral("captionEditor"));
    QVERIFY(editor);

    // Paste has to land in the field (multi-line included), not trigger the
    // window's Paste action and replace the code behind it.
    QTest::keyClick(editor, Qt::Key_V, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("pasted\nvalue"));
    QCOMPARE(window.encodedText(), QStringLiteral("pasted\nvalue"));
}

void TestEnquber::liveReturnClosesTheField()
{
    MainWindow window;
    showAndActivate(&window);

    QTest::keyClick(&window, Qt::Key_L, Qt::ControlModifier);
    auto *editor = window.findChild<TypeEditor *>(QStringLiteral("captionEditor"));
    QVERIFY(editor);
    QTest::keyClicks(editor, QStringLiteral("hello"));
    QVERIFY(editor->isVisible());

    QTest::keyClick(editor, Qt::Key_Return);

    // Return confirms: the field steps aside and the caption takes over.
    QVERIFY(!editor->isVisible());
    QVERIFY(window.hasCode());
    QCOMPARE(window.encodedText(), QStringLiteral("hello"));
    QVERIFY(window.findChild<QLabel *>(QStringLiteral("encodedText"))->isVisible());
    // Confirming hands the code back, so the buttons come alive again, and the
    // keyboard follows the hidden editor to the copy action rather than being
    // left to Qt's own choice of focus target.
    QPushButton *copy = buttonContaining(&window, QStringLiteral("copy"));
    QVERIFY(copy);
    QVERIFY(copy->isEnabled());
    QCOMPARE(QApplication::focusWidget(), copy);
}

void TestEnquber::liveEscapeRestoresThePreviousCode()
{
    MainWindow window;
    showAndActivate(&window);
    window.setText(QStringLiteral("https://keep.example"));

    QTest::keyClick(&window, Qt::Key_L, Qt::ControlModifier);
    auto *editor = window.findChild<TypeEditor *>(QStringLiteral("captionEditor"));
    QVERIFY(editor);
    QVERIFY(editor->isVisible());
    editor->setPlainText(QStringLiteral("https://discard.example"));
    QVERIFY(window.encodedText() == QStringLiteral("https://discard.example"));

    QTest::keyClick(editor, Qt::Key_Escape);

    QVERIFY(!editor->isVisible());
    QVERIFY(window.hasCode());
    QCOMPARE(window.encodedText(), QStringLiteral("https://keep.example"));
    // Cancelling back to an existing code also has to hand the keyboard on.
    QCOMPARE(QApplication::focusWidget(), buttonContaining(&window, QStringLiteral("copy")));
}

void TestEnquber::dropZoneClickOpensTheTypeField()
{
    MainWindow window;
    showAndActivate(&window);

    auto *zone = window.findChild<DropZone *>();
    QVERIFY(zone);
    QTest::mouseClick(zone, Qt::LeftButton);

    auto *editor = window.findChild<TypeEditor *>(QStringLiteral("captionEditor"));
    QVERIFY(editor);
    QVERIFY(editor->isVisible());
    // Clicking the empty target lands in the same placeholder state.
    QVERIFY(!window.hasCode());
    QVERIFY(window.findChild<QrView *>()->hasCode());
}

void TestEnquber::captionEditorMatchesTheSymbolWidth()
{
    MainWindow window;
    showAndActivate(&window);
    window.setText(QStringLiteral("https://width.example"));

    QTest::keyClick(&window, Qt::Key_L, Qt::ControlModifier);
    QCoreApplication::processEvents();

    auto *editor = window.findChild<TypeEditor *>(QStringLiteral("captionEditor"));
    auto *view = window.findChild<QrView *>();
    QVERIFY(editor);
    QVERIFY(view);
    const int side = view->codeSide();
    QVERIFY(side > 0);
    // About as wide as the symbol, not the whole window (allow a pixel of
    // rounding from the whole-device-pixel module size).
    QVERIFY(editor->width() <= side + 2);
    QVERIFY(editor->width() >= side - 2);
    QVERIFY(editor->width() < window.width());
}

void TestEnquber::captionEditorTextIsCentered()
{
    MainWindow window;
    showAndActivate(&window);

    QTest::keyClick(&window, Qt::Key_L, Qt::ControlModifier);
    auto *editor = window.findChild<TypeEditor *>(QStringLiteral("captionEditor"));
    QVERIFY(editor);
    QCOMPARE(int(editor->document()->firstBlock().blockFormat().alignment()),
             int(Qt::AlignHCenter));

    // Setting the text again (the prefill path) must not drop the alignment,
    // and a Ctrl+Enter line must inherit it.
    editor->setPlainText(QStringLiteral("one\ntwo"));
    QTest::keyClick(editor, Qt::Key_Return, Qt::ControlModifier);
    QTest::keyClicks(editor, QStringLiteral("three"));
    for (QTextBlock block = editor->document()->begin(); block.isValid(); block = block.next()) {
        QCOMPARE(int(block.blockFormat().alignment()), int(Qt::AlignHCenter));
    }
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

void TestEnquber::codeSideIsZeroWithoutACode()
{
    QrView view;
    view.resize(300, 300);
    QCOMPARE(view.codeSide(), 0);

    view.setCode(qr::Code::encode(QStringLiteral("https://side.example")));
    QVERIFY(view.codeSide() > 0);

    // A code that failed to encode has nothing to paint either.
    view.setCode(qr::Code());
    QCOMPARE(view.codeSide(), 0);

    view.setCode(qr::Code::encode(QStringLiteral("https://side.example")));
    view.clear();
    QCOMPARE(view.codeSide(), 0);
}

void TestEnquber::codeSideIsTheSizeOfThePaintedSymbol_data()
{
    QTest::addColumn<QSize>("viewSize");
    QTest::addColumn<QString>("text");

    // The symbol is whole modules at whole pixels each, so its side moves in
    // steps as the view grows and as the text needs a bigger symbol.
    const QString small = QStringLiteral("x");
    const QString large = QString(100, QLatin1Char('y'));
    QTest::newRow("square") << QSize(300, 300) << small;
    QTest::newRow("odd square") << QSize(333, 333) << small;
    QTest::newRow("wide, the height limits it") << QSize(600, 301) << small;
    QTest::newRow("tall, the width limits it") << QSize(299, 700) << small;
    QTest::newRow("a larger symbol") << QSize(333, 333) << large;
}

void TestEnquber::codeSideIsTheSizeOfThePaintedSymbol()
{
    QFETCH(QSize, viewSize);
    QFETCH(QString, text);

    QrView view;
    // On a known gray ground the symbol's quiet zone is the only pure white, so
    // its bounding box is the painted symbol. The view's faint outline is made
    // white too: it is drawn half a pixel off the image when the symbol is
    // centered on a half pixel, and would otherwise tint the edge column.
    QPalette palette = view.palette();
    palette.setColor(QPalette::Window, QColor(200, 200, 200));
    palette.setColor(QPalette::WindowText, Qt::white);
    view.setPalette(palette);
    view.setAutoFillBackground(true);
    view.resize(viewSize);
    view.setCode(qr::Code::encode(text));
    QVERIFY(view.hasCode());

    const QImage painted = view.grab().toImage();
    int left = painted.width();
    int top = painted.height();
    int right = -1;
    int bottom = -1;
    for (int y = 0; y < painted.height(); ++y) {
        for (int x = 0; x < painted.width(); ++x) {
            if (painted.pixel(x, y) == qRgb(255, 255, 255)) {
                left = qMin(left, x);
                top = qMin(top, y);
                right = qMax(right, x);
                bottom = qMax(bottom, y);
            }
        }
    }
    QVERIFY2(right >= 0, "nothing white was painted");

    const qreal ratio = painted.devicePixelRatio();
    QCOMPARE(qRound((right - left + 1) / ratio), view.codeSide());
    QCOMPARE(qRound((bottom - top + 1) / ratio), view.codeSide());
}

void TestEnquber::dprChangeRebuildsTheCache()
{
    DprProbeView view;
    // On a known gray ground the symbol's quiet zone is the only pure white, so
    // its bounding box is the painted symbol.
    QPalette palette = view.palette();
    palette.setColor(QPalette::Window, QColor(200, 200, 200));
    palette.setColor(QPalette::WindowText, Qt::white);
    view.setPalette(palette);
    view.setAutoFillBackground(true);
    view.resize(200, 200);

    // A version-40 symbol (padded to 185 modules) in the smallest window: the
    // module size is clamped to 1 at DPR 1 and 2 alike, so only the ratio can
    // invalidate the cache. The module count pins that premise.
    const qr::Code code = qr::Code::encode(QString(1600, QLatin1Char('a')));
    QVERIFY(code.isValid());
    QCOMPARE(code.modules(), 177);
    view.setCode(code);

    const QImage atOne = view.grab().toImage();
    const QRect one = whiteBounds(atOne);
    QVERIFY2(one.isValid(), "nothing white was painted");
    const qreal oneRatio = atOne.devicePixelRatio();
    const int sideAtOne = view.codeSide();
    QCOMPARE(qRound(one.width() / oneRatio), sideAtOne);

    view.probeDpr = 2.0;
    const QImage atTwo = view.grab().toImage();
    const QRect two = whiteBounds(atTwo);
    QVERIFY2(two.isValid(), "nothing white was painted");
    const qreal twoRatio = atTwo.devicePixelRatio();
    const int sideAtTwo = view.codeSide();

    // With the DPR in the cache key the symbol is rebuilt at the new ratio and
    // the painted side follows codeSide(); without it the stale DPR-1 image is
    // reused and the painted side stays ~185 logical px.
    QCOMPARE(qRound(two.width() / twoRatio), sideAtTwo);
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
    bool dialogSeen = false;
    bool dialogAccepted = false;
    QTimer::singleShot(0, &window, [&window, &dialogSeen, &dialogAccepted, path] {
        auto *dialog = window.findChild<QFileDialog *>();
        dialogSeen = dialog != nullptr;
        if (!dialog) {
            return;
        }
        dialog->selectFile(path);
        // accept() is protected, but still a slot, so go through the meta object.
        dialogAccepted = QMetaObject::invokeMethod(dialog, "accept");
    });

    QPushButton *save = buttonContaining(&window, QStringLiteral("save"));
    QVERIFY(save);
    QTest::mouseClick(save, Qt::LeftButton);

    QVERIFY2(dialogSeen, "the save dialog never appeared");
    QVERIFY2(dialogAccepted, "the save dialog could not be accepted");
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

void TestEnquber::saveReportsAWriteFailure()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    MainWindow window;
    showAndActivate(&window);
    window.setText(QStringLiteral("https://save.example"));

    // The target's directory does not exist, so the write has to fail...
    const QString path = directory.filePath(QStringLiteral("missing/code.png"));
    QVERIFY(!window.saveTo(path));
    QVERIFY(!QFileInfo::exists(path));

    // ...and the failure has to reach the status line.
    auto *status = window.findChild<QLabel *>(QStringLiteral("statusLabel"));
    QVERIFY(status);
    QVERIFY(status->isVisible());
    QVERIFY(!status->text().isEmpty());
}

void TestEnquber::exportGeometry_data()
{
    QTest::addColumn<QString>("text");

    QTest::newRow("small symbol") << QStringLiteral("https://save.example");
    QTest::newRow("mid-sized symbol") << QString(100, QLatin1Char('y'));
    QTest::newRow("largest symbol") << QString(1600, QLatin1Char('a'));
}

void TestEnquber::exportGeometry()
{
    QFETCH(QString, text);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    MainWindow window;
    showAndActivate(&window);
    window.setText(text);
    QVERIFY(window.hasCode());

    const QString path = directory.filePath(QStringLiteral("code.png"));
    QVERIFY(window.saveTo(path));

    const QImage image(path);
    QVERIFY(!image.isNull());

    // The expected side is computed from the module count directly, not through
    // modulePixelsFor(), which would only prove the helper agrees with itself.
    const int total = qr::Code::encode(text).totalModules();
    const int expected = total * (1024 / total);
    QCOMPARE(image.width(), expected);
    QCOMPARE(image.height(), expected);

    // The exported PNG carries the 300 dpi promise, and it survives the file.
    QCOMPARE(image.dotsPerMeterX(), 11811);
    QCOMPARE(image.dotsPerMeterY(), 11811);
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
    // The zone only paints the window's highlight; it must not accept drops
    // itself, or Qt would route a drag over it away from the one protocol.
    QVERIFY(!window.findChild<DropZone *>()->acceptDrops());
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
    PaletteStateRestorer restore;
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
    PaletteStateRestorer restore;
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

    PaletteStateRestorer restore;
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
    PaletteStateRestorer restore;
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

void TestEnquber::aboutPageShowsLicenseAndLinks()
{
    MainWindow window;
    showAndActivate(&window);

    auto *copyright = window.findChild<QLabel *>(QStringLiteral("aboutCopyright"));
    QVERIFY(copyright);
    QVERIFY(copyright->text().contains(QStringLiteral("Morgan Astra")));

    auto *license = window.findChild<QLabel *>(QStringLiteral("aboutLicense"));
    QVERIFY(license);
    QVERIFY(license->text().contains(QStringLiteral("GNU General Public License")));

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

    auto *license = window.findChild<QLabel *>(QStringLiteral("aboutLicense"));
    QVERIFY(license);
    QVERIFY(license->isVisible());

    // A short window used to squash the wrapped license to a single clipped
    // line; it now keeps the height its text needs at its fixed width.
    const int needed = license->heightForWidth(license->width());
    QVERIFY(needed > 0);
    QVERIFY2(license->height() >= needed,
             qPrintable(QStringLiteral("license is %1 px, needs %2")
                            .arg(license->height()).arg(needed)));
}

void TestEnquber::resizeWhileHelpIsUpReshapesTheCaption()
{
    MainWindow window;
    showAndActivate(&window);
    const QString words = QStringLiteral("word ").repeated(30);
    window.setText(words);

    auto *label = window.findChild<QLabel *>(QStringLiteral("encodedText"));
    QVERIFY(label);
    const int wide = label->width();
    // Two lines at the initial width.
    QCOMPARE(label->text().count(QLatin1Char('\n')), 1);

    // The caption is on the hidden code page while help is up; the window
    // resize must still leave the label shaped for the width it will have
    // when the page comes back.
    QTest::keyClick(&window, Qt::Key_H, Qt::ControlModifier);
    window.resize(360, 500);
    QCoreApplication::processEvents();
    QTest::keyClick(&window, Qt::Key_Escape);
    QCoreApplication::processEvents();

    QVERIFY(label->width() < wide);
    // Three lines now: the label's own Resize filter re-shaped it.
    QCOMPARE(label->text().count(QLatin1Char('\n')), 2);
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
    // A few representative IDs. tools/check-i18n.py and Qt's lcheck own the
    // full source-to-catalog sync, so this test only has to prove the compiled
    // catalog resolves messages at runtime, never to the id itself.
    const QStringList ids = {
        QStringLiteral("app.name"),
        QStringLiteral("dropzone.title"),
        QStringLiteral("typeeditor.placeholder"),
        QStringLiteral("qrcode.error.too-much-data"),
        QStringLiteral("about.links"),
    };

    for (const QString &id : ids) {
        const QString text = qtTrId(id.toUtf8().constData());
        QVERIFY2(!text.isEmpty(), qPrintable(id));
        QVERIFY2(text != id, qPrintable(id));
    }
}

/// Sending a drop straight to the window bypasses the drag target selection, so
/// these cases pin the state transitions the drag-and-drop path takes while
/// another mode is up.
void TestEnquber::dropWhileTypingReplacesTheEdit()
{
    MainWindow window;
    showAndActivate(&window);
    window.setText(QStringLiteral("https://before.example"));

    QTest::keyClick(&window, Qt::Key_L, Qt::ControlModifier);
    auto *editor = window.findChild<TypeEditor *>(QStringLiteral("captionEditor"));
    QVERIFY(editor);
    QTest::keyClicks(editor, QStringLiteral("https://typed.example"));
    QCOMPARE(window.encodedText(), QStringLiteral("https://typed.example"));

    QScopedPointer<QMimeData> mime(textMime(QStringLiteral("https://dropped.example")));
    performDrop(&window, mime.data());

    QVERIFY(!editor->isVisible());
    QCOMPARE(window.encodedText(), QStringLiteral("https://dropped.example"));
    QPushButton *copy = buttonContaining(&window, QStringLiteral("copy"));
    QVERIFY(copy);
    QVERIFY(copy->isEnabled());
}

void TestEnquber::helpWhileTypingCancelsTheEditAndComesBack()
{
    MainWindow window;
    showAndActivate(&window);
    window.setText(QStringLiteral("https://keep.example"));

    QTest::keyClick(&window, Qt::Key_L, Qt::ControlModifier);
    auto *editor = window.findChild<TypeEditor *>(QStringLiteral("captionEditor"));
    QVERIFY(editor);
    QTest::keyClicks(editor, QStringLiteral("https://discard.example"));

    auto *stack = window.findChild<QStackedWidget *>();
    QTest::keyClick(&window, Qt::Key_H, Qt::ControlModifier);
    QCOMPARE(stack->currentIndex(), 2);
    QVERIFY(!editor->isVisible());
    QCOMPARE(window.encodedText(), QStringLiteral("https://keep.example"));

    QTest::keyClick(&window, Qt::Key_Escape);
    QCOMPARE(stack->currentIndex(), 1);
    QCOMPARE(window.encodedText(), QStringLiteral("https://keep.example"));
    QPushButton *copy = buttonContaining(&window, QStringLiteral("copy"));
    QVERIFY(copy);
    QVERIFY(copy->isEnabled());
}

void TestEnquber::committingAnEmptyEditGoesBackToTheDropTarget()
{
    MainWindow window;
    showAndActivate(&window);
    window.setText(QStringLiteral("https://before.example"));

    QTest::keyClick(&window, Qt::Key_L, Qt::ControlModifier);
    auto *editor = window.findChild<TypeEditor *>(QStringLiteral("captionEditor"));
    QVERIFY(editor);
    editor->clear();
    QTest::keyClick(editor, Qt::Key_Return);

    QVERIFY(!editor->isVisible());
    QVERIFY(!window.hasCode());
    QCOMPARE(window.findChild<QStackedWidget *>()->currentIndex(), 0);
    QPushButton *copy = buttonContaining(&window, QStringLiteral("copy"));
    QVERIFY(copy);
    QVERIFY(!copy->isEnabled());
}

void TestEnquber::escapeWithNoPreviousCodeReturnsToTheDropTarget()
{
    MainWindow window;
    showAndActivate(&window);

    QTest::keyClick(&window, Qt::Key_L, Qt::ControlModifier);
    auto *editor = window.findChild<TypeEditor *>(QStringLiteral("captionEditor"));
    QVERIFY(editor);
    QTest::keyClicks(editor, QStringLiteral("typed"));
    QVERIFY(window.hasCode());

    QTest::keyClick(editor, Qt::Key_Escape);
    QVERIFY(!editor->isVisible());
    QVERIFY(!window.hasCode());
    QCOMPARE(window.findChild<QStackedWidget *>()->currentIndex(), 0);
    QVERIFY(window.findChild<DropZone *>()->isVisible());
}

int main(int argc, char *argv[])
{
    // findChild<QFileDialog *>() cannot see a native dialog, and a modal native
    // save dialog would block the test instead of failing it.
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    // The rest mirrors QTEST_MAIN, which cannot set the attribute above early
    // enough to be usable here.
    app.setAttribute(Qt::AA_Use96Dpi, true);
    TestEnquber test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_enquber.moc"
