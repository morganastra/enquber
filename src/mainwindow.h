#pragma once

#include "mimetext.h"
#include "qrcode.h"

#include <QMainWindow>
#include <QTemporaryDir>

class QDir;

class AboutPage;
class DropZone;
class QrView;
class QAbstractButton;
class QAction;
class QLabel;
class QPushButton;
class QStackedWidget;
class QTimer;
class QDragEnterEvent;
class QDragLeaveEvent;
class QDropEvent;
class QEvent;

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

    /// Shows the help / about page, or returns from it when it is already up.
    void toggleAbout();

    /// Returns from the help / about page to whichever page it was opened from.
    void closeAbout();

private:
    enum Page { PlaceholderPage, CodePage, HelpPage };

    void buildUi();
    void buildActions();
    /// (Re)builds the action icons. The bundled fallback glyphs are tinted with
    /// the palette when they are created, so this has to run again whenever the
    /// palette changes; theme icons simply ignore the call.
    void refreshActionIcons();
    void showAbout();
    void showCode(const qr::Code &code);
    void showPlaceholder();
    /// Enables/disables the Copy, Save and Clear actions together.
    void setCodeActionsEnabled(bool enabled);
    void updateTextLabel();
    void showStatus(const QString &message);
    void clearStatus();
    void setDropHighlight(bool active);
    void positionHelpButton();
    QString suggestedFileName() const;
    QImage renderForExport() const;
    /// Starts dragging the current code out to another application. The payload
    /// and the temporary file it points at are prepared here, because the
    /// window owns export and naming.
    void startCodeDrag();
    /// Writes @p image to a fresh file under the session temp directory, so a
    /// file manager or the desktop has something real to save. Returns the path,
    /// or an empty string when the file could not be written.
    QString writeDragFile(const QImage &image);
    /// True when @p event belongs to a drag that started inside this window, so
    /// the window does not offer to encode its own dragged image back over the
    /// code.
    bool dragFromThisWindow(const QDropEvent *event) const;

    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

    QStackedWidget *m_stack = nullptr;
    AboutPage *m_aboutPage = nullptr;
    QWidget *m_central = nullptr;
    DropZone *m_dropZone = nullptr;
    QrView *m_qrView = nullptr;
    QLabel *m_textLabel = nullptr;
    QLabel *m_statusLabel = nullptr;
    QTimer *m_statusTimer = nullptr;
    QAbstractButton *m_helpButton = nullptr;
    QPushButton *m_copyButton = nullptr;
    QPushButton *m_saveButton = nullptr;
    QPushButton *m_clearButton = nullptr;
    QAction *m_pasteAction = nullptr;
    QAction *m_copyAction = nullptr;
    QAction *m_saveAction = nullptr;
    QAction *m_clearAction = nullptr;
    QAction *m_helpAction = nullptr;
    QAction *m_closeAboutAction = nullptr;
    QAction *m_quitAction = nullptr;

    Page m_pageBeforeAbout = PlaceholderPage;
    bool m_aboutOpen = false;
    int m_shapedWidth = -1;

    qr::Code m_code;

    mime::Payload m_payload;

    /// Holds the PNGs the code is dragged as; wiped when the window goes away.
    /// Built explicitly so that the directory names itself after the app.
    QTemporaryDir m_dragDir {QDir(QDir::tempPath()).filePath(QStringLiteral("enquber-XXXXXX")) };
    int m_dragCount = 0;
};
