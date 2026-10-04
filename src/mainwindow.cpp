#include "mainwindow.h"

#include "aboutpage.h"
#include "dropzone.h"
#include "mimeimage.h"
#include "mimetext.h"
#include "qrview.h"
#include "theme.h"

#include <QAbstractButton>
#include <QAction>
#include <QClipboard>
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
#include <QTextLayout>
#include <QTimer>
#include <QtGlobal>
#include <QUrl>
#include <QVBoxLayout>

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
constexpr int kDragLingerMs = 1000;

constexpr int kStatusTimeoutMs = 4000;

constexpr int kMaxTextLines = 3;

constexpr int kWindowMargin = 24;

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
            const QPointF centre = circle.center();
            const qreal half = 4.5;
            painter.setPen(QPen(glyph, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.setBrush(Qt::NoBrush);
            painter.drawLine(QPointF(centre.x() + half, centre.y()), QPointF(centre.x() - half, centre.y()));
            painter.drawLine(QPointF(centre.x() - half, centre.y()),
                             QPointF(centre.x() - half + 4.0, centre.y() - 4.0));
            painter.drawLine(QPointF(centre.x() - half, centre.y()),
                             QPointF(centre.x() - half + 4.0, centre.y() + 4.0));
        } else {
            QFont question = font();
            question.setBold(true);
            painter.setFont(question);
            painter.setPen(glyph);
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
/// Newlines are honoured explicitly: a QLabel breaks on them, QTextLayout does
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
    setWindowTitle(tr("Enquber"));
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
    connect(m_dropZone, &DropZone::textDropped, this, &MainWindow::setText);
    m_stack->addWidget(placeholderPage);

    auto *codePage = new QWidget(m_stack);
    auto *codeLayout = new QVBoxLayout(codePage);
    codeLayout->setContentsMargins(0, 0, 0, 0);
    codeLayout->setSpacing(14);

    m_qrView = new QrView(codePage);
    codeLayout->addWidget(m_qrView, 1);
    connect(m_qrView, &QrView::dragRequested, this, &MainWindow::startCodeDrag);

    m_textLabel = new QLabel(codePage);
    m_textLabel->setObjectName(QStringLiteral("encodedText"));
    m_textLabel->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    m_textLabel->setWordWrap(true);
    m_textLabel->setTextFormat(Qt::PlainText);
    m_textLabel->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    m_textLabel->setFocusPolicy(Qt::ClickFocus);
    m_textLabel->installEventFilter(this);
    codeLayout->addWidget(m_textLabel);

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
    m_helpButton->setToolTip(tr("Show help and info (Ctrl+H or ?)"));
    m_helpButton->setAccessibleName(tr("Help and info"));
    connect(m_helpButton, &QAbstractButton::clicked, this, &MainWindow::toggleAbout);
    central->installEventFilter(this);
    positionHelpButton();

    setCentralWidget(central);
}

void MainWindow::buildActions()
{
    m_pasteAction = new QAction(tr("&Paste link"), this);
    m_pasteAction->setShortcut(QKeySequence::Paste);
    m_pasteAction->setShortcutContext(Qt::WindowShortcut);
    connect(m_pasteAction, &QAction::triggered, this, &MainWindow::pasteFromClipboard);
    addAction(m_pasteAction);

    m_copyAction = new QAction(tr("&Copy image"), this);
    m_copyAction->setShortcut(QKeySequence::Copy);
    m_copyAction->setToolTip(tr("Copy the QR code to the clipboard as a PNG image (Ctrl+C)"));
    connect(m_copyAction, &QAction::triggered, this, &MainWindow::copyToClipboard);
    addAction(m_copyAction);

    m_saveAction = new QAction(tr("&Save…"), this);
    m_saveAction->setShortcut(QKeySequence::Save);
    m_saveAction->setToolTip(tr("Save the QR code as a PNG file (Ctrl+S)"));
    connect(m_saveAction, &QAction::triggered, this, &MainWindow::askWhereToSave);
    addAction(m_saveAction);

    m_copyButton->setText(m_copyAction->text());
    m_copyButton->setToolTip(m_copyAction->toolTip());
    connect(m_copyButton, &QPushButton::clicked, m_copyAction, &QAction::trigger);

    m_saveButton->setText(m_saveAction->text());
    m_saveButton->setToolTip(m_saveAction->toolTip());
    connect(m_saveButton, &QPushButton::clicked, m_saveAction, &QAction::trigger);

    m_clearAction = new QAction(tr("C&lear"), this);
    m_clearAction->setShortcuts({QKeySequence(Qt::Key_Escape), QKeySequence(Qt::Key_Backspace),
                                 QKeySequence(Qt::Key_Delete)});
    m_clearAction->setToolTip(tr("Go back to the drop target (Esc, Backspace or Delete)"));
    connect(m_clearAction, &QAction::triggered, this, &MainWindow::showPlaceholder);
    addAction(m_clearAction);

    m_clearButton->setText(m_clearAction->text());
    m_clearButton->setToolTip(m_clearAction->toolTip());
    connect(m_clearButton, &QPushButton::clicked, m_clearAction, &QAction::trigger);

    m_helpAction = new QAction(tr("&Help and info"), this);
    m_helpAction->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_H), QKeySequence(Qt::Key_Question)});
    m_helpAction->setToolTip(tr("Show help and info (Ctrl+H or ?)"));
    connect(m_helpAction, &QAction::triggered, this, &MainWindow::toggleAbout);
    addAction(m_helpAction);

    // Escape leaves the help page when it is up. It is enabled only then, so it
    // never competes with the clear action's own Escape shortcut.
    m_closeAboutAction = new QAction(this);
    m_closeAboutAction->setShortcut(QKeySequence(Qt::Key_Escape));
    m_closeAboutAction->setShortcutContext(Qt::WindowShortcut);
    m_closeAboutAction->setEnabled(false);
    connect(m_closeAboutAction, &QAction::triggered, this, &MainWindow::closeAbout);
    addAction(m_closeAboutAction);

    m_quitAction = new QAction(tr("&Quit"), this);
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
    m_quitAction->setToolTip(tr("Quit enquber (Ctrl+Q)"));
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

    qr::Code code = qr::Code::encode(trimmed);
    if (!code.isValid()) {
        showStatus(code.error());
        return;
    }

    m_code = code;
    showCode(m_code);
}

void MainWindow::pasteFromClipboard()
{
    const QString text = mime::textForQr(QGuiApplication::clipboard()->mimeData());
    if (text.isEmpty()) {
        showStatus(tr("The clipboard holds no text or link"));
        return;
    }
    setText(text);
}

void MainWindow::showCode(const qr::Code &code)
{
    if (m_aboutOpen) {
        closeAbout();
    }
    m_qrView->setCode(code);
    m_stack->setCurrentIndex(CodePage);
    updateTextLabel();
    m_copyAction->setEnabled(true);
    m_saveAction->setEnabled(true);
    m_clearAction->setEnabled(true);

    m_copyButton->setFocus(Qt::OtherFocusReason);
    clearStatus();
}

void MainWindow::showPlaceholder()
{
    if (m_aboutOpen) {
        closeAbout();
    }
    m_code = qr::Code();
    m_payload.forget();
    m_qrView->clear();
    m_textLabel->clear();
    m_stack->setCurrentIndex(PlaceholderPage);
    m_copyAction->setEnabled(false);
    m_saveAction->setEnabled(false);
    m_clearAction->setEnabled(false);
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
    m_aboutOpen = true;
    m_pageBeforeAbout = static_cast<Page>(m_stack->currentIndex());

    m_stack->setCurrentIndex(HelpPage);

    // Leaving is the corner button's job while the page is up.
    static_cast<HelpButton *>(m_helpButton)->setBack(true);
    m_helpButton->setToolTip(tr("Back to Enquber (Esc or Ctrl+H)"));
    m_helpButton->setAccessibleName(tr("Back to Enquber"));

    // The page is read-only, so nothing below it should act on the code.
    m_pasteAction->setEnabled(false);
    m_copyAction->setEnabled(false);
    m_saveAction->setEnabled(false);
    m_clearAction->setEnabled(false);
    m_closeAboutAction->setEnabled(true);

    clearStatus();
    m_helpButton->setFocus(Qt::OtherFocusReason);
}

void MainWindow::closeAbout()
{
    if (!m_aboutOpen) {
        return;
    }
    m_aboutOpen = false;

    static_cast<HelpButton *>(m_helpButton)->setBack(false);
    m_helpButton->setToolTip(tr("Show help and info (Ctrl+H or ?)"));
    m_helpButton->setAccessibleName(tr("Help and info"));

    m_closeAboutAction->setEnabled(false);
    m_pasteAction->setEnabled(true);

    m_stack->setCurrentIndex(m_pageBeforeAbout);

    const bool hasCode = m_code.isValid();
    m_copyAction->setEnabled(hasCode);
    m_saveAction->setEnabled(hasCode);
    m_clearAction->setEnabled(hasCode);

    if (m_pageBeforeAbout == CodePage && hasCode) {
        m_copyButton->setFocus(Qt::OtherFocusReason);
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

    auto *data = new QMimeData;
    data->setImageData(image);
    data->setData(QStringLiteral("image/png"), mime::encodePng(image));
    QGuiApplication::clipboard()->setMimeData(data);

    showStatus(tr("Copied the QR code to the clipboard"));
}

void MainWindow::startCodeDrag()
{
    if (!m_code.isValid()) {
        return;
    }

    const QImage image = renderForExport();
    const QString path = writeDragFile(image);
    if (path.isEmpty()) {
        showStatus(tr("Could not prepare the image for dragging"));
        return;
    }

    auto *drag = new QDrag(m_qrView);
    drag->setMimeData(mime::payloadForDrag(image, path));
    // A small copy of the symbol follows the cursor. Nearest-neighbour scaling
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

    const QString path = QFileDialog::getSaveFileName(this,
                                                      tr("Save QR Code"),
                                                      QDir(directory).filePath(suggestedFileName()),
                                                      tr("PNG image (*.png)"));
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
        showStatus(tr("Could not write %1").arg(QDir::toNativeSeparators(target)));
        return false;
    }

    showStatus(tr("Saved to %1").arg(QDir::toNativeSeparators(target)));
    return true;
}

QImage MainWindow::renderForExport() const
{
    const int modules = m_code.modules() + 2 * qr::Code::QuietZone;
    const int modulePixels = std::max(4, kExportPixels / modules);
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
    const QString path = QDir(directory).filePath(suggestedFileName());
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
    stem.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")), QStringLiteral("-"));
    stem.remove(QRegularExpression(QStringLiteral("^[.-]+|[.-]+$")));
    if (stem.isEmpty()) {
        stem = QStringLiteral("qrcode");
    }
    return stem.left(60) + QStringLiteral(".png");
}

void MainWindow::showStatus(const QString &message)
{
    m_statusLabel->setText(message);
    m_statusLabel->setVisible(!message.isEmpty());

    if (message.isEmpty()) {
        m_statusTimer->stop();
    } else {
        m_statusTimer->start(kStatusTimeoutMs);
    }
}

void MainWindow::clearStatus()
{
    m_statusLabel->clear();
    m_statusLabel->hide();
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
    if (m_payload.observe(event->mimeData()).isEmpty()) {
        qCDebug(lcDnd) << "nothing to encode, ignoring it";
        return;
    }
    event->acceptProposedAction();
    setDropHighlight(true);
    if (m_code.isValid()) {
        showStatus(tr("Drop to replace the current code"));
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
    if (!m_payload.observe(event->mimeData()).isEmpty()) {
        event->acceptProposedAction();
    }
}

void MainWindow::dragLeaveEvent(QDragLeaveEvent *event)
{
    qCDebug(lcDnd) << "drag leave";
    m_payload.forget();
    QMainWindow::dragLeaveEvent(event);
    setDropHighlight(false);
    clearStatus();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    if (dragFromThisWindow(event)) {
        return;
    }
    qCDebug(lcDnd) << "drop with" << event->mimeData()->formats() << "at" << event->position();
    setDropHighlight(false);
    clearStatus();

    const QString text = m_payload.resolve(event->mimeData());
    m_payload.forget();
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
