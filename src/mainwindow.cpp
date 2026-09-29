#include "mainwindow.h"

#include "dropzone.h"
#include "mimetext.h"
#include "qrview.h"
#include "theme.h"

#include <QAction>
#include <QBuffer>
#include <QClipboard>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLoggingCategory>
#include <QMimeData>
#include <QPushButton>
#include <QRegularExpression>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTextLayout>
#include <QTimer>
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

constexpr int kStatusTimeoutMs = 4000;

constexpr int kMaxTextLines = 3;

constexpr int kWindowMargin = 24;

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
    setWindowIcon(theme::icon({"view-barcode-qr", "view-barcode"}));
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

    m_textLabel = new QLabel(codePage);
    m_textLabel->setObjectName(QStringLiteral("encodedText"));
    m_textLabel->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    m_textLabel->setWordWrap(true);
    m_textLabel->setTextFormat(Qt::PlainText);
    m_textLabel->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    m_textLabel->setFocusPolicy(Qt::ClickFocus);
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

    auto *central = new QWidget(this);
    auto *centralLayout = new QVBoxLayout(central);
    centralLayout->setContentsMargins(kWindowMargin, kWindowMargin, kWindowMargin, 12);
    centralLayout->setSpacing(10);
    centralLayout->addWidget(m_stack, 1);

    m_statusTimer = new QTimer(this);
    m_statusTimer->setObjectName(QStringLiteral("statusTimeout"));
    m_statusTimer->setSingleShot(true);
    connect(m_statusTimer, &QTimer::timeout, this, &MainWindow::clearStatus);

    m_statusLabel = new QLabel(central);
    m_statusLabel->setAlignment(Qt::AlignCenter);
    m_statusLabel->setTextFormat(Qt::PlainText);
    m_statusLabel->setWordWrap(true);
    QPalette statusPalette = m_statusLabel->palette();
    statusPalette.setColor(QPalette::WindowText, statusPalette.color(QPalette::PlaceholderText));
    m_statusLabel->setPalette(statusPalette);
    centralLayout->addWidget(m_statusLabel);

    setCentralWidget(central);
}

void MainWindow::buildActions()
{
    m_pasteAction = new QAction(tr("&Paste link"), this);
    m_pasteAction->setShortcut(QKeySequence::Paste);
    m_pasteAction->setShortcutContext(Qt::WindowShortcut);
    connect(m_pasteAction, &QAction::triggered, this, &MainWindow::pasteFromClipboard);
    addAction(m_pasteAction);

    m_copyAction = new QAction(theme::icon({"edit-copy"}), tr("&Copy image"), this);
    m_copyAction->setShortcut(QKeySequence::Copy);
    m_copyAction->setToolTip(tr("Copy the QR code to the clipboard as a PNG image (Ctrl+C)"));
    connect(m_copyAction, &QAction::triggered, this, &MainWindow::copyToClipboard);
    addAction(m_copyAction);

    m_saveAction = new QAction(theme::icon({"document-save", "document-save-as"}), tr("&Save…"), this);
    m_saveAction->setShortcut(QKeySequence::Save);
    m_saveAction->setToolTip(tr("Save the QR code as a PNG file (Ctrl+S)"));
    connect(m_saveAction, &QAction::triggered, this, &MainWindow::askWhereToSave);
    addAction(m_saveAction);

    m_copyButton->setText(m_copyAction->text());
    m_copyButton->setIcon(m_copyAction->icon());
    m_copyButton->setToolTip(m_copyAction->toolTip());
    connect(m_copyButton, &QPushButton::clicked, m_copyAction, &QAction::trigger);

    m_saveButton->setText(m_saveAction->text());
    m_saveButton->setIcon(m_saveAction->icon());
    m_saveButton->setToolTip(m_saveAction->toolTip());
    connect(m_saveButton, &QPushButton::clicked, m_saveAction, &QAction::trigger);

    m_clearAction = new QAction(theme::icon({"edit-clear", "edit-clear-all", "window-close"}),
                                tr("C&lear"), this);
    m_clearAction->setShortcuts({QKeySequence(Qt::Key_Escape), QKeySequence(Qt::Key_Backspace),
                                 QKeySequence(Qt::Key_Delete)});
    m_clearAction->setToolTip(tr("Go back to the drop target (Esc, Backspace or Delete)"));
    connect(m_clearAction, &QAction::triggered, this, &MainWindow::showPlaceholder);
    addAction(m_clearAction);

    m_clearButton->setText(m_clearAction->text());
    m_clearButton->setIcon(m_clearAction->icon());
    m_clearButton->setToolTip(m_clearAction->toolTip());
    connect(m_clearButton, &QPushButton::clicked, m_clearAction, &QAction::trigger);
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

void MainWindow::updateTextLabel()
{
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
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");

    auto *data = new QMimeData;
    data->setImageData(image);
    data->setData(QStringLiteral("image/png"), png);
    QGuiApplication::clipboard()->setMimeData(data);

    showStatus(tr("Copied the QR code to the clipboard"));
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

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
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
