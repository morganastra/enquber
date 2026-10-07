#pragma once

#include "mimetext.h"
#include "qrcode.h"

#include <QMainWindow>
#include <QPoint>
#include <QTemporaryDir>

#include <cstdint>

class QDir;

class AboutPage;
class DropZone;
class QrView;
class TypeEditor;
class QAbstractButton;
class QAction;
class QContextMenuEvent;
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

    [[nodiscard]] bool hasCode() const { return m_code.isValid(); }
    [[nodiscard]] QString encodedText() const { return m_code.text(); }

public slots:
    /// Encodes @p text (typically a URL) and shows the result. Empty input is
    /// ignored, input that does not fit is reported in the status line.
    void setText(const QString &text);

    /// Encodes whatever the clipboard holds, if it holds anything usable.
    void pasteFromClipboard();

    /// Opens the inline multi-line field under the QR.
    void typeText();

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
    enum Page : std::uint8_t { PlaceholderPage, CodePage, HelpPage };

    void buildUi();
    void buildActions();
    /// (Re)builds the action icons. The bundled fallback glyphs are tinted with
    /// the palette when they are created, so this has to run again whenever the
    /// palette changes; theme icons simply ignore the call.
    void refreshActionIcons();
    void showAbout();
    void showCode(const qr::Code &code);
    /// Makes @p code the current one and shows the code page: the single
    /// "present a finished code" transition. Callers end any inline edit first.
    void presentCode(const qr::Code &code);
    void showPlaceholder();
    /// Puts the multi-line field in the caption slot under the QR view.
    void beginLiveInput();
    /// Rebuilds the symbol from the field's text (run on every keystroke), or
    /// shows the placeholder while the field is empty.
    void liveEncode();
    /// Return accepts the typed text and closes the field; empty text returns
    /// to the placeholder, and text that does not encode keeps the field open.
    void commitLiveInput();
    /// Escape closes the field and restores the code from before editing.
    void cancelLiveInput();
    /// Rests and hides the field, restoring the normal actions.
    void finishTypeInput();
    /// Matches the field's width to the symbol above it.
    void positionCaptionEditor();
    /// Builds and runs the window's context menu at @p globalPos. Every item is
    /// one of the window's own actions, so icons, shortcut hints, enabled
    /// states and translations all follow the one action. The set follows the
    /// page: Paste and Type everywhere except the help page, Copy/Save/Clear on
    /// the code page, Back to Enquber on the help page, plus Help and Quit. An
    /// open inline field is committed first, the way Return does, so the menu
    /// describes the finished code with every action live.
    void showContextMenu(const QPoint &globalPos);
    /// Brings every action in line with the current state: Paste and the code
    /// actions follow the code, the inline field and the help page; Type stays
    /// on unless the help page is up; Escape belongs to the help page only
    /// while it is. The buttons mirror Copy/Save/Clear. Call it after the state
    /// changes and before any setFocus(): a disabled button refuses focus.
    void updateActionStates();
    void updateTextLabel();
    /// Encodes @p text and reports a failure in the status line. Empty input is
    /// the caller's business, because each entry point treats it differently.
    [[nodiscard]] qr::Code encodeOrReport(const QString &text);
    void showStatus(const QString &message);
    void clearStatus();
    void setDropHighlight(bool active);
    void positionHelpButton();
    /// Points the corner button at the info page or back at the app, with the
    /// matching tooltip and accessible name.
    void setHelpButtonBack(bool back);
    [[nodiscard]] QString suggestedFileName() const;
    [[nodiscard]] QImage renderForExport() const;
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
    void contextMenuEvent(QContextMenuEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

    QStackedWidget *m_stack = nullptr;
    AboutPage *m_aboutPage = nullptr;
    QWidget *m_central = nullptr;
    DropZone *m_dropZone = nullptr;
    QrView *m_qrView = nullptr;
    QLabel *m_textLabel = nullptr;
    TypeEditor *m_captionEditor = nullptr;
    QLabel *m_statusLabel = nullptr;
    QTimer *m_statusTimer = nullptr;
    QAbstractButton *m_helpButton = nullptr;
    QPushButton *m_copyButton = nullptr;
    QPushButton *m_saveButton = nullptr;
    QPushButton *m_clearButton = nullptr;
    QAction *m_pasteAction = nullptr;
    QAction *m_typeAction = nullptr;
    QAction *m_copyAction = nullptr;
    QAction *m_saveAction = nullptr;
    QAction *m_clearAction = nullptr;
    QAction *m_helpAction = nullptr;
    QAction *m_closeAboutAction = nullptr;
    QAction *m_quitAction = nullptr;

    Page m_pageBeforeAbout = PlaceholderPage;
    bool m_aboutOpen = false;
    /// What was encoded before the inline editor took over the window; Escape
    /// puts it back.
    qr::Code m_codeBeforeType;
    bool m_typeInputActive = false;
    /// Set while the caption field is filled in programmatically, so that does
    /// not look like the user typing and trigger a live rebuild.
    bool m_liveSuppress = false;
    int m_shapedWidth = -1;

    qr::Code m_code;

    mime::ObservedText m_dropText;
    /// True while the status line shows the "drop to replace" message this
    /// drag put there, so only that message is cleared when the drag ends.
    bool m_dropStatusShown = false;

    /// Holds the PNGs the code is dragged as; wiped when the window goes away.
    /// Built explicitly so that the directory names itself after the app.
    QTemporaryDir m_dragDir {QDir(QDir::tempPath()).filePath(QStringLiteral("enquber-XXXXXX")) };
    int m_dragCount = 0;
};
