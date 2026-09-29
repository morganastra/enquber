#pragma once

#include "mimetext.h"
#include "qrcode.h"

#include <QMainWindow>

class DropZone;
class QrView;
class QAction;
class QLabel;
class QPushButton;
class QStackedWidget;
class QTimer;
class QDragEnterEvent;
class QDragLeaveEvent;
class QDropEvent;

/// The application window: an empty drop target until something is pasted or
/// dropped, then the resulting QR code with its text and the export buttons.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

    bool hasCode() const { return m_code.isValid(); }
    QString encodedText() const { return m_code.text(); }

public slots:
    /// Encodes @p text (typically a URL) and shows the result. Empty input is
    /// ignored, input that does not fit is reported in the status line.
    void setText(const QString &text);

    /// Encodes whatever the clipboard holds, if it holds anything usable.
    void pasteFromClipboard();

    /// Copies the current code to the clipboard as a PNG image.
    void copyToClipboard();

    /// Asks for a file name and writes the current code there.
    void askWhereToSave();

    /// Writes the current code as a PNG to @p path, appending the .png suffix
    /// when the name has none. Returns false and reports the failure when the
    /// file could not be written.
    bool saveTo(const QString &path);

private:
    enum Page { PlaceholderPage, CodePage };

    void buildUi();
    void buildActions();
    void showCode(const qr::Code &code);
    void showPlaceholder();
    void updateTextLabel();
    void showStatus(const QString &message);
    void clearStatus();
    void setDropHighlight(bool active);
    QString suggestedFileName() const;
    QImage renderForExport() const;

    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

    QStackedWidget *m_stack = nullptr;
    DropZone *m_dropZone = nullptr;
    QrView *m_qrView = nullptr;
    QLabel *m_textLabel = nullptr;
    QLabel *m_statusLabel = nullptr;
    QTimer *m_statusTimer = nullptr;
    QPushButton *m_copyButton = nullptr;
    QPushButton *m_saveButton = nullptr;
    QPushButton *m_clearButton = nullptr;
    QAction *m_pasteAction = nullptr;
    QAction *m_copyAction = nullptr;
    QAction *m_saveAction = nullptr;
    QAction *m_clearAction = nullptr;

    qr::Code m_code;

    /// What the drag currently hovering over the window is offering.
    mime::Payload m_payload;
};
