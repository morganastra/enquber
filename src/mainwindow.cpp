#include "mainwindow.h"

#include "aboutpage.h"
#include "dropzone.h"
#include "mimeimage.h"
#include "mimetext.h"
#include "qrview.h"
#include "theme.h"
#include "typeeditor.h"

#include <QAbstractButton>
#include <QAction>
#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEnterEvent>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLoggingCategory>
#include <QMimeData>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTextDocument>
#include <QTextLayout>
#include <QTimer>
#include <QtGlobal>
#include <QUrl>
#include <QVBoxLayout>

#include <chrono>

/// Set QT_LOGGING_RULES="enquber.dnd.debug=true" to watch what the window is
/// offered while something is dragged onto it.
Q_LOGGING_CATEGORY(lcDnd, "enquber.dnd")

namespace {

/// Side of the exported PNG in pixels; module size is rounded down so the
/// result stays a whole number of pixels per module.
constexpr int kExportPixels = 1024;

/// 300 dpi, so that printing the PNG gets a sensible physical size.
constexpr int kDotsPerMeter = 11811;

/// Side of the pixmap that follows the cursor while the code is dragged out.
constexpr int kDragPixmapPixels = 120;

/// How long the finished drag is kept alive so a target can still fetch the
/// payload over the X11 selection after the drop. See startCodeDrag().
constexpr std::chrono::milliseconds kDragLingerMs{1000};

constexpr std::chrono::milliseconds kStatusTimeoutMs{4000};

constexpr int kMaxTextLines = 3;

constexpr int kWindowMargin = 24;

/// The symbol shown under an empty inline field, so the code page never looks
/// blank at the moment the user is about to type. It is data, not UI text, so
/// it is deliberately not translated.
constexpr QLatin1StringView kPlaceholderText("enquber");

/// Distance of the help button from the top-right corner of the window.
constexpr int kHelpButtonMargin = 10;

/// A small, deliberately quiet circular "?" that opens the help / about page.
/// It is painted rather than styled so it follows the palette (and stays round
/// under every platform style).
class HelpButton : public QAbstractButton
{
public:
    explicit HelpButton(QWidget *parent = nullptr)
        : QAbstractButton(parent)
    {
        setFixedSize(28, 28);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
    }

    /// While the help page is up the button stops asking a question and points
    /// back to where the user came from.
    void setBack(bool back)
    {
        if (m_back == back) {
            return;
        }
        m_back = back;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QRectF circle = QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0);
        const bool lit = underMouse() || hasFocus();

        const QColor highlight = palette().color(QPalette::Highlight);
        const QColor border = lit ? highlight : palette().color(QPalette::Mid);
        QColor glyph = lit ? highlight : palette().color(QPalette::WindowText);
        if (!lit) {
            glyph.setAlphaF(0.55);
        }
        QColor fill = palette().color(QPalette::Base);
        fill.setAlphaF(underMouse() ? 0.85 : 0.5);

        painter.setPen(QPen(border, 1.0));
        painter.setBrush(fill);
        painter.drawEllipse(circle);

        if (m_back) {
            const QPointF center = circle.center();
            const qreal half = 4.5;
            painter.setPen(QPen(glyph, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.setBrush(Qt::NoBrush);
            painter.drawLine(QPointF(center.x() + half, center.y()), QPointF(center.x() - half, center.y()));
            painter.drawLine(QPointF(center.x() - half, center.y()),
                             QPointF(center.x() - half + 4.0, center.y() - 4.0));
            painter.drawLine(QPointF(center.x() - half, center.y()),
                             QPointF(center.x() - half + 4.0, center.y() + 4.0));
        } else {
            QFont question = font();
            question.setBold(true);
            painter.setFont(question);
            painter.setPen(glyph);
            // A drawn symbol, deliberately not translated.
            painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("?"));
        }
    }

    void enterEvent(QEnterEvent *event) override
    {
        QAbstractButton::enterEvent(event);
        update();
    }

    void leaveEvent(QEvent *event) override
    {
        QAbstractButton::leaveEvent(event);
        update();
    }

private:
    bool m_back = false;
};

/// The text as the label should show it, and whether anything had to be left
/// out (in which case the whole text belongs in the tooltip).
struct ShapedText
{
    QString text;
    bool truncated = false;
};

/// Word wraps @p text into at most @p maxLines lines of @p width pixels.
///
/// Newlines are honored explicitly: a QLabel breaks on them, QTextLayout does
/// not, so laying the whole string out would count a paragraph of six lines as
/// one and hand it back untouched. Wrapping inside long unbreakable tokens is
/// allowed for the same reason, otherwise a link without spaces comes back as a
/// single line far wider than the label. When the last line is cut, an ellipsis
/// is added to it.
ShapedText shapeForLabel(const QString &text, const QFont &font, const QFontMetrics &metrics,
                         int width, int maxLines)
{
    ShapedText shaped;
    if (width <= 0 || maxLines < 1) {
        shaped.text = text;
        return shaped;
    }

    QStringList lines;
    bool truncated = false;

    const QStringList paragraphs = text.split(QLatin1Char('\n'));
    for (const QString &candidate : paragraphs) {
        if (lines.size() >= maxLines) {
            truncated = true; // another paragraph that will not be shown
            break;
        }
        QString paragraph = candidate;
        if (paragraph.endsWith(QLatin1Char('\r'))) {
            paragraph.chop(1);
        }

        QTextLayout layout(paragraph, font);
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        layout.setTextOption(option);
        layout.beginLayout();

        int consumed = 0;
        while (lines.size() < maxLines) {
            QTextLine line = layout.createLine();
            if (!line.isValid()) {
                break;
            }
            line.setLineWidth(width);
            consumed = line.textStart() + line.textLength();
            lines.append(paragraph.mid(line.textStart(), line.textLength()));
        }
        layout.endLayout();

        if (!paragraph.mid(consumed).trimmed().isEmpty()) {
            truncated = true; // the rest of this paragraph will not be shown
            break;
        }
    }

    if (truncated && !lines.isEmpty() && !lines.last().endsWith(QChar(0x2026))) {
        // Make the cut visible even when the last line happens to be short.
        lines.last() = metrics.elidedText(lines.last() + QChar(0x2026), Qt::ElideRight, width);
    }

    shaped.text = lines.join(QLatin1Char('\n'));
    shaped.truncated = truncated;
    return shaped;
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    //@ App
    //% "Enquber"
    setWindowTitle(qtTrId("app.name"));
    setWindowIcon(theme::appIcon());
    setAcceptDrops(true);
    resize(560, 700);

    buildUi();
    buildActions();
    showPlaceholder();
}

void MainWindow::buildUi()
{
    m_stack = new QStackedWidget(this);

    auto *placeholderPage = new QWidget(m_stack);
    auto *placeholderLayout = new QVBoxLayout(placeholderPage);
    placeholderLayout->setContentsMargins(0, 0, 0, 0);
    placeholderLayout->addStretch(1);
    m_dropZone = new DropZone(placeholderPage);
    placeholderLayout->addWidget(m_dropZone, 0, Qt::AlignCenter);
    placeholderLayout->addStretch(1);
    connect(m_dropZone, &DropZone::clicked, this, &MainWindow::typeText);
    m_stack->addWidget(placeholderPage);

    auto *codePage = new QWidget(m_stack);
    auto *codeLayout = new QVBoxLayout(codePage);
    codeLayout->setContentsMargins(0, 0, 0, 0);
    codeLayout->setSpacing(14);

    m_qrView = new QrView(codePage);
    codeLayout->addWidget(m_qrView, 1);
    connect(m_qrView, &QrView::dragRequested, this, &MainWindow::startCodeDrag);
    m_qrView->installEventFilter(this);

    m_textLabel = new QLabel(codePage);
    m_textLabel->setObjectName(QStringLiteral("encodedText"));
    m_textLabel->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    m_textLabel->setWordWrap(true);
    m_textLabel->setTextFormat(Qt::PlainText);
    m_textLabel->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    m_textLabel->setFocusPolicy(Qt::ClickFocus);
    m_textLabel->installEventFilter(this);
    codeLayout->addWidget(m_textLabel);

    // The inline editor, in the caption slot under the QR view. It is swapped
    // with m_textLabel while editing so the symbol stays on screen and rebuilds
    // as the text changes. Its width is matched to the symbol in
    // positionCaptionEditor().
    m_captionEditor = new TypeEditor(codePage);
    m_captionEditor->setObjectName(QStringLiteral("captionEditor"));
    // About three lines tall: enough to show a small multi-line payload without
    // the field dominating the window. Anything longer scrolls.
    const QFontMetrics editorMetrics(m_captionEditor->fontMetrics());
    constexpr int kEditorLines = 3;
    m_captionEditor->setFixedHeight(kEditorLines * editorMetrics.lineSpacing()
                                    + 2 * m_captionEditor->frameWidth()
                                    + 2 * qRound(m_captionEditor->document()->documentMargin())
                                    + 2);
    m_captionEditor->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    //@ TypeEditor
    //% "Type or paste text"
    m_captionEditor->setCenteredPlaceholder(qtTrId("typeeditor.placeholder"));
    m_captionEditor->hide();
    connect(m_captionEditor, &TypeEditor::submitted, this, &MainWindow::commitLiveInput);
    connect(m_captionEditor, &TypeEditor::cancelled, this, &MainWindow::cancelLiveInput);
    connect(m_captionEditor, &QTextEdit::textChanged, this, &MainWindow::liveEncode);
    codeLayout->addWidget(m_captionEditor, 0, Qt::AlignHCenter);

    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(12);
    buttons->addStretch(1);
    m_copyButton = new QPushButton(codePage);
    m_saveButton = new QPushButton(codePage);
    m_clearButton = new QPushButton(codePage);
    buttons->addWidget(m_copyButton);
    buttons->addWidget(m_saveButton);

    buttons->addSpacing(24);
    buttons->addWidget(m_clearButton);
    buttons->addStretch(1);
    codeLayout->addLayout(buttons);

    m_stack->addWidget(codePage);

    m_aboutPage = new AboutPage(m_stack);
    m_stack->addWidget(m_aboutPage);

    m_central = new QWidget(this);
    auto *central = m_central;
    auto *centralLayout = new QVBoxLayout(central);
    centralLayout->setContentsMargins(kWindowMargin, kWindowMargin, kWindowMargin, 12);
    centralLayout->setSpacing(10);
    centralLayout->addWidget(m_stack, 1);

    m_statusTimer = new QTimer(this);
    m_statusTimer->setObjectName(QStringLiteral("statusTimeout"));
    m_statusTimer->setSingleShot(true);
    connect(m_statusTimer, &QTimer::timeout, this, &MainWindow::clearStatus);

    m_statusLabel = new QLabel(central);
    m_statusLabel->setObjectName(QStringLiteral("statusLabel"));
    m_statusLabel->setAlignment(Qt::AlignCenter);
    m_statusLabel->setTextFormat(Qt::PlainText);
    m_statusLabel->setWordWrap(true);
    // Same secondary-text role as the drop-zone hint: resolved at paint time,
    // so the status line keeps up with a light/dark theme switch.
    m_statusLabel->setForegroundRole(QPalette::PlaceholderText);
    centralLayout->addWidget(m_statusLabel);

    // The help button floats over the stack in the window's upper-right corner
    // instead of taking a slot in the layout, so it does not shift the content
    // the mock-ups were drawn against.
    m_helpButton = new HelpButton(central);
    m_helpButton->setObjectName(QStringLiteral("helpButton"));
    //: Tooltip on the floating help button; it also opens the help page with
    //: the keyboard shortcuts Ctrl+H or the "?" key.
    //@ MainWindow
    //% "Show help and info (Ctrl+H or ?)"
    m_helpButton->setToolTip(qtTrId("mainwindow.help.tooltip"));
    //@ MainWindow
    //% "Help and info"
    m_helpButton->setAccessibleName(qtTrId("mainwindow.help.accessible"));
    connect(m_helpButton, &QAbstractButton::clicked, this, &MainWindow::toggleAbout);
    central->installEventFilter(this);
    positionHelpButton();

    setCentralWidget(central);
}

void MainWindow::buildActions()
{
    //@ MainWindow
    //% "&Paste link"
    m_pasteAction = new QAction(qtTrId("mainwindow.action.paste"), this);
    m_pasteAction->setShortcut(QKeySequence::Paste);
    m_pasteAction->setShortcutContext(Qt::WindowShortcut);
    connect(m_pasteAction, &QAction::triggered, this, &MainWindow::pasteFromClipboard);
    addAction(m_pasteAction);

    //@ MainWindow
    //% "&Type text…"
    m_typeAction = new QAction(qtTrId("mainwindow.action.type"), this);
    m_typeAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_L));
    m_typeAction->setShortcutContext(Qt::WindowShortcut);
    //@ MainWindow
    //% "Type the text to encode (Ctrl+L)"
    m_typeAction->setToolTip(qtTrId("mainwindow.action.type.tooltip"));
    connect(m_typeAction, &QAction::triggered, this, &MainWindow::typeText);
    addAction(m_typeAction);

    //@ MainWindow
    //% "&Copy image"
    m_copyAction = new QAction(qtTrId("mainwindow.action.copy"), this);
    m_copyAction->setShortcut(QKeySequence::Copy);
    //@ MainWindow
    //% "Copy the QR code to the clipboard as a PNG image (Ctrl+C)"
    m_copyAction->setToolTip(qtTrId("mainwindow.action.copy.tooltip"));
    connect(m_copyAction, &QAction::triggered, this, &MainWindow::copyToClipboard);
    addAction(m_copyAction);

    //@ MainWindow
    //% "&Save…"
    m_saveAction = new QAction(qtTrId("mainwindow.action.save"), this);
    m_saveAction->setShortcut(QKeySequence::Save);
    //@ MainWindow
    //% "Save the QR code as a PNG file (Ctrl+S)"
    m_saveAction->setToolTip(qtTrId("mainwindow.action.save.tooltip"));
    connect(m_saveAction, &QAction::triggered, this, &MainWindow::askWhereToSave);
    addAction(m_saveAction);

    m_copyButton->setText(m_copyAction->text());
    m_copyButton->setToolTip(m_copyAction->toolTip());
    connect(m_copyButton, &QPushButton::clicked, m_copyAction, &QAction::trigger);

    m_saveButton->setText(m_saveAction->text());
    m_saveButton->setToolTip(m_saveAction->toolTip());
    connect(m_saveButton, &QPushButton::clicked, m_saveAction, &QAction::trigger);

    //@ MainWindow
    //% "C&lear"
    m_clearAction = new QAction(qtTrId("mainwindow.action.clear"), this);
    m_clearAction->setShortcuts({QKeySequence(Qt::Key_Escape), QKeySequence(Qt::Key_Backspace),
                                 QKeySequence(Qt::Key_Delete)});
    //@ MainWindow
    //% "Go back to the drop target (Esc, Backspace or Delete)"
    m_clearAction->setToolTip(qtTrId("mainwindow.action.clear.tooltip"));
    connect(m_clearAction, &QAction::triggered, this, &MainWindow::showPlaceholder);
    addAction(m_clearAction);

    m_clearButton->setText(m_clearAction->text());
    m_clearButton->setToolTip(m_clearAction->toolTip());
    connect(m_clearButton, &QPushButton::clicked, m_clearAction, &QAction::trigger);

    //@ MainWindow
    //% "&Help and info"
    m_helpAction = new QAction(qtTrId("mainwindow.action.help"), this);
    m_helpAction->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_H), QKeySequence(Qt::Key_Question)});
    //@ MainWindow
    //% "Show help and info (Ctrl+H or ?)"
    m_helpAction->setToolTip(qtTrId("mainwindow.help.tooltip"));
    connect(m_helpAction, &QAction::triggered, this, &MainWindow::toggleAbout);
    addAction(m_helpAction);

    // Escape leaves the help page when it is up. It is enabled only then, so it
    // never competes with the clear action's own Escape shortcut.
    m_closeAboutAction = new QAction(this);
    m_closeAboutAction->setShortcut(QKeySequence(Qt::Key_Escape));
    m_closeAboutAction->setShortcutContext(Qt::WindowShortcut);
    connect(m_closeAboutAction, &QAction::triggered, this, &MainWindow::closeAbout);
    addAction(m_closeAboutAction);

    //@ MainWindow
    //% "&Quit"
    m_quitAction = new QAction(qtTrId("mainwindow.action.quit"), this);
    // The platform's standard quit gesture (Ctrl+Q on Linux and Windows, Cmd+Q
    // on macOS) plus Ctrl+Q itself, so the shortcut also works where the theme
    // leaves the standard key unbound. Identical sequences are added once: two
    // copies of the same key would make the shortcut ambiguous and it would
    // never fire.
    QList<QKeySequence> quitShortcuts;
    const QKeySequence standardQuit(QKeySequence::Quit);
    if (!standardQuit.isEmpty()) {
        quitShortcuts.append(standardQuit);
    }
    const QKeySequence controlQ(QStringLiteral("Ctrl+Q"));
    if (!quitShortcuts.contains(controlQ)) {
        quitShortcuts.append(controlQ);
    }
    m_quitAction->setShortcuts(quitShortcuts);
    m_quitAction->setShortcutContext(Qt::WindowShortcut);
    //@ MainWindow
    //% "Quit enquber (Ctrl+Q)"
    m_quitAction->setToolTip(qtTrId("mainwindow.action.quit.tooltip"));
    connect(m_quitAction, &QAction::triggered, this, &QWidget::close);
    addAction(m_quitAction);

    refreshActionIcons();
}

void MainWindow::refreshActionIcons()
{
    if (!m_copyAction) {
        return; // a palette change before buildActions() finished
    }
    // theme::icon() tints a bundled fallback with the current palette when it
    // builds it, so a palette change means the glyphs have to be built again.
    // System theme icons are drawn by the theme and simply ignore this.
    m_copyAction->setIcon(theme::icon({"edit-copy"}));
    m_saveAction->setIcon(theme::icon({"document-save", "document-save-as"}));
    m_clearAction->setIcon(theme::icon({"edit-clear", "edit-clear-all", "window-close"}));

    m_copyButton->setIcon(m_copyAction->icon());
    m_saveButton->setIcon(m_saveAction->icon());
    m_clearButton->setIcon(m_clearAction->icon());
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange) {
        refreshActionIcons();
    }
}

void MainWindow::setText(const QString &text)
{
    qCDebug(lcDnd) << "asked to encode" << text.size() << "characters";
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }

    qr::Code code = encodeOrReport(trimmed);
    if (!code.isValid()) {
        return;
    }

    m_code = std::move(code);
    showCode(m_code);
}

void MainWindow::pasteFromClipboard()
{
    const QString text = mime::textForQr(QGuiApplication::clipboard()->mimeData());
    if (text.isEmpty()) {
        //@ MainWindow
        //% "The clipboard holds no text or link"
        showStatus(qtTrId("mainwindow.status.clipboard-empty"));
        return;
    }
    setText(text);
}

void MainWindow::typeText()
{
    beginLiveInput();
}

void MainWindow::beginLiveInput()
{
    if (m_aboutOpen) {
        return;
    }
    if (m_typeInputActive) {
        return;
    }
    m_typeInputActive = true;
    m_codeBeforeType = m_code;
    // The QR view has to be on screen, so this editor lives on the code page
    // even when there is no symbol yet.
    m_stack->setCurrentIndex(CodePage);

    m_liveSuppress = true;
    m_captionEditor->setPlainText(m_code.isValid() ? m_code.text() : QString());
    m_liveSuppress = false;

    m_textLabel->hide();
    m_captionEditor->show();
    positionCaptionEditor();
    m_captionEditor->setFocus(Qt::OtherFocusReason);
    m_captionEditor->selectAll();

    // While the field owns the keyboard, Escape/Backspace/Delete (Clear) and
    // Ctrl+V/Ctrl+C (Paste/Copy) belong to the editor, not to the window
    // actions. The next transition (commit, cancel or placeholder) puts them
    // back through updateActionStates().
    updateActionStates();
    clearStatus();

    // Even with nothing typed yet, show a symbol so the page is not blank.
    liveEncode();
}

void MainWindow::liveEncode()
{
    if (m_liveSuppress || !m_typeInputActive) {
        return;
    }

    const QString trimmed = m_captionEditor->toPlainText().trimmed();
    if (trimmed.isEmpty()) {
        // Nothing typed: stand in with a placeholder symbol so the code page
        // still reads as a QR maker rather than a blank text box.
        m_code = qr::Code();
        m_qrView->setCode(qr::Code::encode(kPlaceholderText));
        positionCaptionEditor();
        clearStatus();
        return;
    }

    qr::Code code = encodeOrReport(trimmed);
    if (!code.isValid()) {
        // Keep the last good symbol on screen and explain why this one cannot
        // be shown; the field keeps whatever the user typed.
        return;
    }

    // Copy/Save/Clear stay disabled while the field is up: their shortcuts
    // (Ctrl+C/S, Esc) have to reach the editor instead. commitLiveInput() and
    // cancelLiveInput() decide their final state.
    m_code = code;
    m_qrView->setCode(code);
    positionCaptionEditor();
    clearStatus();
}

void MainWindow::commitLiveInput()
{
    if (!m_typeInputActive) {
        return;
    }

    const QString trimmed = m_captionEditor->toPlainText().trimmed();
    if (trimmed.isEmpty()) {
        showPlaceholder();
        return;
    }

    qr::Code code = encodeOrReport(trimmed);
    if (!code.isValid()) {
        // Too much to encode: stay in the field with the text intact.
        return;
    }

    finishTypeInput();
    presentCode(code);
}

void MainWindow::cancelLiveInput()
{
    if (!m_typeInputActive) {
        return;
    }
    finishTypeInput();

    if (m_codeBeforeType.isValid()) {
        presentCode(m_codeBeforeType);
    } else {
        showPlaceholder();
    }
}

void MainWindow::finishTypeInput()
{
    m_typeInputActive = false;
    if (m_captionEditor->isVisible()) {
        m_captionEditor->hide();
        m_textLabel->show();
    }
}

void MainWindow::positionCaptionEditor()
{
    const int side = m_qrView->codeSide();
    if (side > 0) {
        m_captionEditor->setFixedWidth(side);
    }
}

void MainWindow::updateActionStates()
{
    const bool editing = m_typeInputActive;
    const bool reading = m_aboutOpen;
    const bool canExport = m_code.isValid() && !editing && !reading;

    m_pasteAction->setEnabled(!editing && !reading);
    // Deliberately on while typing: Ctrl+L is a no-op there, not a key to steal.
    m_typeAction->setEnabled(!reading);
    m_closeAboutAction->setEnabled(reading);

    m_copyAction->setEnabled(canExport);
    m_saveAction->setEnabled(canExport);
    m_clearAction->setEnabled(canExport);
    m_copyButton->setEnabled(canExport);
    m_saveButton->setEnabled(canExport);
    m_clearButton->setEnabled(canExport);
}

void MainWindow::presentCode(const qr::Code &code)
{
    m_code = code;
    m_qrView->setCode(code);
    m_stack->setCurrentIndex(CodePage);
    updateTextLabel();
    updateActionStates();

    // The editor (or whatever had the keyboard) is hidden or about to lose it,
    // so hand the keyboard to the action most people want next. This has to
    // stay after updateActionStates(): a disabled button silently refuses
    // focus, and the keyboard would land somewhere surprising (the floating
    // help button, say).
    m_copyButton->setFocus(Qt::OtherFocusReason);
    clearStatus();
}

void MainWindow::showCode(const qr::Code &code)
{
    if (m_aboutOpen) {
        closeAbout();
    }
    finishTypeInput();
    presentCode(code);
}

void MainWindow::showPlaceholder()
{
    if (m_aboutOpen) {
        closeAbout();
    }
    m_code = qr::Code();
    m_dropText.forget();
    finishTypeInput();
    m_qrView->clear();
    m_textLabel->clear();
    m_stack->setCurrentIndex(PlaceholderPage);
    updateActionStates();
    clearStatus();
}

void MainWindow::toggleAbout()
{
    if (m_aboutOpen) {
        closeAbout();
    } else {
        showAbout();
    }
}

void MainWindow::showAbout()
{
    if (m_aboutOpen) {
        return;
    }
    cancelLiveInput();
    m_aboutOpen = true;
    m_pageBeforeAbout = static_cast<Page>(m_stack->currentIndex());

    m_stack->setCurrentIndex(HelpPage);

    // Leaving is the corner button's job while the page is up.
    setHelpButtonBack(true);

    // The page is read-only, so nothing below it should act on the code.
    updateActionStates();

    clearStatus();
    m_helpButton->setFocus(Qt::OtherFocusReason);
}

void MainWindow::closeAbout()
{
    if (!m_aboutOpen) {
        return;
    }
    m_aboutOpen = false;

    setHelpButtonBack(false);

    m_stack->setCurrentIndex(m_pageBeforeAbout);
    updateActionStates();

    const bool hasCode = m_code.isValid();
    if (m_pageBeforeAbout == CodePage && hasCode) {
        m_copyButton->setFocus(Qt::OtherFocusReason);
    }
}

void MainWindow::setHelpButtonBack(bool back)
{
    auto *button = static_cast<HelpButton *>(m_helpButton);
    button->setBack(back);
    if (back) {
        //@ MainWindow
        //% "Back to Enquber (Esc or Ctrl+H)"
        button->setToolTip(qtTrId("mainwindow.help.back.tooltip"));
        //@ MainWindow
        //% "Back to Enquber"
        button->setAccessibleName(qtTrId("mainwindow.help.back.accessible"));
    } else {
        //@ MainWindow
        //% "Show help and info (Ctrl+H or ?)"
        button->setToolTip(qtTrId("mainwindow.help.tooltip"));
        //@ MainWindow
        //% "Help and info"
        button->setAccessibleName(qtTrId("mainwindow.help.accessible"));
    }
}

void MainWindow::positionHelpButton()
{
    if (!m_helpButton || !m_central) {
        return;
    }
    m_helpButton->move(m_central->width() - m_helpButton->width() - kHelpButtonMargin,
                       kHelpButtonMargin);
    m_helpButton->raise();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::Resize) {
        if (watched == m_central) {
            positionHelpButton();
        } else if (watched == m_qrView) {
            // Keep the field as wide as the symbol above it.
            positionCaptionEditor();
        } else if (watched == m_textLabel && m_textLabel->width() != m_shapedWidth) {
            // The first shape happens before the layout has settled, so the
            // label can still be narrow; re-shape it once it has its real width.
            updateTextLabel();
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::updateTextLabel()
{
    m_shapedWidth = m_textLabel->width();
    if (!m_code.isValid()) {
        m_textLabel->clear();
        return;
    }
    const QString text = m_code.text();
    const QFontMetrics metrics(m_textLabel->fontMetrics());
    const ShapedText shaped = shapeForLabel(text, m_textLabel->font(), metrics,
                                            m_textLabel->width(), kMaxTextLines);
    m_textLabel->setText(shaped.text);
    m_textLabel->setToolTip(shaped.truncated ? Qt::convertFromPlainText(text) : QString());
}

void MainWindow::copyToClipboard()
{
    if (!m_code.isValid()) {
        return;
    }

    const QImage image = renderForExport();
    QGuiApplication::clipboard()->setMimeData(mime::imagePayload(image));

    //@ MainWindow
    //% "Copied the QR code to the clipboard"
    showStatus(qtTrId("mainwindow.status.copied"));
}

void MainWindow::startCodeDrag()
{
    if (!m_code.isValid()) {
        return;
    }

    const QImage image = renderForExport();
    const QString path = writeDragFile(image);
    if (path.isEmpty()) {
        //@ MainWindow
        //% "Could not prepare the image for dragging"
        showStatus(qtTrId("mainwindow.status.drag-failed"));
        return;
    }

    auto *drag = new QDrag(m_qrView);
    drag->setMimeData(mime::payloadForDrag(image, path));
    // A small copy of the symbol follows the cursor. Nearest-neighbor scaling
    // keeps the modules square instead of blurring them, and the ratio keeps
    // its size honest on a scaled display.
    QPixmap preview = QPixmap::fromImage(image.scaled(kDragPixmapPixels, kDragPixmapPixels,
                                                      Qt::KeepAspectRatio,
                                                      Qt::FastTransformation));
    preview.setDevicePixelRatio(m_qrView->devicePixelRatioF());
    drag->setPixmap(preview);
    // The hot spot is in logical pixels, but pixmap() reports device pixels once
    // a ratio is set, so the size has to come from the device independent one:
    // Qt's own documented pixmap().width() / 2 would put the cursor in the
    // preview's bottom right corner on a scaled display.
    const QSizeF logical = preview.deviceIndependentSize();
    drag->setHotSpot(QPoint(qRound(logical.width() / 2), qRound(logical.height() / 2)));

    qCDebug(lcDnd) << "dragging the code out of the window";
    const Qt::DropAction action = drag->exec(Qt::CopyAction);
    qCDebug(lcDnd) << "drag finished with action" << action;

    // exec() returns as soon as the drop is delivered, but the target fetches
    // the payload (text/uri-list, image/png) from us afterwards, over the X11
    // selection. Dropping the drag here would drop that selection with it and
    // the target would receive an empty payload, so it is kept alive briefly
    // and then deleted once the transfer has certainly finished.
    QTimer::singleShot(kDragLingerMs, drag, [drag] { drag->deleteLater(); });
}

void MainWindow::askWhereToSave()
{
    if (!m_code.isValid()) {
        return;
    }

    QString directory = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (directory.isEmpty()) {
        directory = QDir::homePath();
    }

    //: Title of the file chooser that saves the QR code as an image.
    //@ MainWindow
    //% "Save QR Code"
    const QString title = qtTrId("mainwindow.dialog.save.title");
    //: File-type filter in the save dialog; the star and the extension must
    //: stay unchanged so Qt can match PNG files.
    //@ MainWindow
    //% "PNG image (*.png)"
    const QString filter = qtTrId("mainwindow.dialog.save.filter");
    const QString path = QFileDialog::getSaveFileName(this,
                                                      title,
                                                      QDir(directory).filePath(suggestedFileName()),
                                                      filter);
    if (path.isEmpty()) {
        return;
    }
    saveTo(path);
}

bool MainWindow::saveTo(const QString &path)
{
    if (!m_code.isValid()) {
        return false;
    }

    QString target = path;
    if (QFileInfo(target).suffix().isEmpty()) {
        target += QStringLiteral(".png");
    }

    const QImage image = renderForExport();
    if (!image.save(target, "PNG")) {
        //: %1 is the path the code could not be written to.
        //@ MainWindow
        //% "Could not write %1"
        showStatus(qtTrId("mainwindow.status.write-failed").arg(QDir::toNativeSeparators(target)));
        return false;
    }

    //: %1 is the path the code was written to.
    //@ MainWindow
    //% "Saved to %1"
    showStatus(qtTrId("mainwindow.status.saved-to").arg(QDir::toNativeSeparators(target)));
    return true;
}

QImage MainWindow::renderForExport() const
{
    // The floor of 4 is unreachable: the largest symbol pads to 185 modules, so 1024 / 185 is 5.
    const int modulePixels = m_code.modulePixelsFor(kExportPixels, 4);
    QImage image = m_code.toImage(modulePixels);
    image.setDotsPerMeterX(kDotsPerMeter);
    image.setDotsPerMeterY(kDotsPerMeter);
    return image;
}

QString MainWindow::writeDragFile(const QImage &image)
{
    // A failed temp directory would send the path relative to the working
    // directory, dropping PNGs beside wherever the app was started.
    if (!m_dragDir.isValid()) {
        return {};
    }
    // A fresh subdirectory per drag keeps a target that reads the file lazily
    // (or a macOS file promise) from tripping over the next drag's file, while
    // the file inside keeps the pretty name the user expects to find after a
    // drop. The whole directory goes away with the window.
    const QString directory = QDir(m_dragDir.path()).filePath(QString::number(++m_dragCount));
    if (!QDir().mkpath(directory)) {
        return {};
    }
    QString path = QDir(directory).filePath(suggestedFileName());
    if (!image.save(path, "PNG")) {
        return {};
    }
    return path;
}

QString MainWindow::suggestedFileName() const
{
    QString stem = m_code.text();
    const QUrl url(stem);
    if (url.isValid() && !url.host().isEmpty()) {
        stem = url.host() + url.path();
    }
    stem = stem.section(QLatin1Char('?'), 0, 0);
    static const QRegularExpression unsafeFileNameChars(QStringLiteral("[^A-Za-z0-9._-]+"));
    static const QRegularExpression leadingTrailingDots(QStringLiteral("^[.-]+|[.-]+$"));
    stem.replace(unsafeFileNameChars, QStringLiteral("-"));
    stem.remove(leadingTrailingDots);
    if (stem.isEmpty()) {
        stem = QStringLiteral("qrcode");
    }
    return stem.left(60) + QStringLiteral(".png");
}

qr::Code MainWindow::encodeOrReport(const QString &text)
{
    qr::Code code = qr::Code::encode(text);
    if (!code.isValid()) {
        showStatus(code.error());
    }
    return code;
}

void MainWindow::showStatus(const QString &message)
{
    m_statusLabel->setText(message);
    m_statusLabel->setVisible(!message.isEmpty());

    // start() also restarts an active one-shot timer, so a repeated message
    // gets its full time instead of the remainder of the previous one.
    if (message.isEmpty()) {
        m_statusTimer->stop();
    } else {
        m_statusTimer->start(kStatusTimeoutMs);
    }
}

void MainWindow::clearStatus()
{
    showStatus(QString());
}

void MainWindow::setDropHighlight(bool active)
{
    m_qrView->setHighlighted(active);
    m_dropZone->setActive(active);
}

bool MainWindow::dragFromThisWindow(const QDropEvent *event) const
{
    // External drags have no source widget. A drag started elsewhere in this
    // process (the smoke test's helper) has one that is not our child, so only
    // our own code is ignored.
    QWidget *source = qobject_cast<QWidget *>(event->source());
    return source && isAncestorOf(source);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (dragFromThisWindow(event)) {
        qCDebug(lcDnd) << "ignoring a drag that started in this window";
        return;
    }
    qCDebug(lcDnd) << "drag enter with" << event->mimeData()->formats();
    if (m_dropText.observe(event->mimeData()).isEmpty()) {
        qCDebug(lcDnd) << "nothing to encode, ignoring it";
        return;
    }
    event->acceptProposedAction();
    setDropHighlight(true);
    m_dropStatusShown = m_code.isValid();
    if (m_dropStatusShown) {
        //@ MainWindow
        //% "Drop to replace the current code"
        showStatus(qtTrId("mainwindow.status.drop-replace"));
    }
}

void MainWindow::dragMoveEvent(QDragMoveEvent *event)
{
    if (dragFromThisWindow(event)) {
        return;
    }
    qCDebug(lcDnd) << "drag move" << event->position();
    // Qt only delivers the drop if the drag was still accepted at the position
    // the button was released on, and an ignored move ends the drag.
    if (!m_dropText.observe(event->mimeData()).isEmpty()) {
        event->acceptProposedAction();
    }
}

void MainWindow::dragLeaveEvent(QDragLeaveEvent *event)
{
    qCDebug(lcDnd) << "drag leave";
    m_dropText.forget();
    QMainWindow::dragLeaveEvent(event);
    setDropHighlight(false);
    // Only a drag that put the "drop to replace" message on screen may take a
    // status away; a refused drag must leave an unrelated message alone.
    if (m_dropStatusShown) {
        clearStatus();
    }
    m_dropStatusShown = false;
}

void MainWindow::dropEvent(QDropEvent *event)
{
    if (dragFromThisWindow(event)) {
        return;
    }
    qCDebug(lcDnd) << "drop with" << event->mimeData()->formats() << "at" << event->position();
    setDropHighlight(false);
    if (m_dropStatusShown) {
        clearStatus();
    }
    m_dropStatusShown = false;

    const QString text = m_dropText.resolve(event->mimeData());
    m_dropText.forget();
    if (text.isEmpty()) {
        qCDebug(lcDnd) << "the payload holds no text";
        return;
    }
    qCDebug(lcDnd) << "encoding" << text;
    event->acceptProposedAction();
    setText(text);
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    updateTextLabel();
}
