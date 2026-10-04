# Dragging the generated QR code out of Enquber

Status: implemented, with unit and GUI smoke-test coverage.

This document describes how Enquber lets a user drag the generated QR code out
of the window and drop it into another application, for example onto the desktop
or into a file manager (which saves it as a PNG) or into a document (which
inserts the image).

## Goal

Dragging the code should behave the way a user already expects dragging an
*image file* to behave. That expectation is what makes this worth doing: real
users try to drag the visible QR code and are surprised when nothing happens.

Different targets want different things, so a single drag has to offer all of
them at once:

| Target | What it looks for | Consequence |
| --- | --- | --- |
| Desktop, Files / Nautilus / Dolphin / Thunar | `text/uri-list` pointing at an **existing local file** | a real PNG must be written to disk before the drag starts; image-only drags create nothing |
| LibreOffice Writer/Impress, GIMP, Krita, browsers | `image/png` / `image/*` (embedded bytes) | `setImageData()` plus a raw `image/png` entry |
| Text editors, address bars | `text/plain` | Qt fills it from the file URL (see below) |
| Rich-text fields | `text/html` | deliberately **not** offered; see "Decisions" |

## Interaction

The drag starts from `QrView`, the widget that shows the symbol. A left press on
the code followed by a move past `QApplication::startDragDistance()` starts a
`QDrag` carrying a `CopyAction` only.

`QrView` owns the gesture but not the payload: it emits
`dragRequested()` and `MainWindow` (which owns naming and export) builds the
`QMimeData` and runs the drag. This split is what makes the gesture unit
testable without starting a real, blocking drag.

The pointer turns into an open hand over the code. There are deliberately no
tooltips or status hints: discoverability was never the problem, users were
already trying to drag the code; it just did not do anything.

```cpp
// QrView
void QrView::mouseMoveEvent(QMouseEvent *event)
{
    if (m_pressed && (event->buttons() & Qt::LeftButton)
        && (event->position().toPoint() - m_pressPos).manhattanLength()
               >= QApplication::startDragDistance()) {
        m_pressed = false;                 // one drag per press
        Q_EMIT dragRequested();
    }
    QWidget::mouseMoveEvent(event);
}
```

`m_pressed` is tracked explicitly rather than read from `event->buttons()` alone,
so that a test can drive the gesture; requiring the held button as well keeps the
check honest for real input. The unit test asserts both: a move past the
threshold with the button held starts a drag, and one without the button does
not.

## Payload

```cpp
// MainWindow::startCodeDrag(), connected to QrView::dragRequested
const QImage image = renderForExport();      // ~1024 px, 300 dpi metadata
const QString path = writeDragFile(image);   // real file, see "Temp file"

auto *drag = new QDrag(m_qrView);
drag->setMimeData(mime::payloadForDrag(image, path));

QPixmap preview = QPixmap::fromImage(image.scaled(120, 120, Qt::KeepAspectRatio,
                                                  Qt::FastTransformation));
preview.setDevicePixelRatio(m_qrView->devicePixelRatioF());
drag->setPixmap(preview);
// The hot spot is logical pixels; the pixmap reports device pixels, so the
// size has to come from the device independent one (see below).
const QSizeF logical = preview.deviceIndependentSize();
drag->setHotSpot(QPoint(qRound(logical.width() / 2), qRound(logical.height() / 2)));

drag->exec(Qt::CopyAction);
```

The hot spot is in **logical** pixels, but `QPixmap::width()` reports device
pixels once a ratio is set. Qt's own documented `pixmap().width() / 2` therefore
puts the cursor at the preview's bottom-right corner on a scaled display; the
device independent size is the value that centres it.

`mime::payloadForDrag()` (in `src/mimeimage.cpp`) builds the object:

- `setImageData(image)` and `setData("image/png", …)` — documents and image
  editors embed the pixels (the same payload `copyToClipboard()` uses).
- `setUrls({QUrl::fromLocalFile(path)})` — file managers and the desktop save a
  copy of the file.
- Plain text is **not** set: since Qt 5, `setUrls()` makes `hasText()` true and
  `text()` fall back to the file URL, so a text editor pastes
  `file:///tmp/enquber-…/name.png`. That is the accepted "drag a file" behaviour.

The PNG is rendered once with `renderForExport()`, so a dropped file carries the
same ~1024 px, 300 dpi image that Ctrl+C and Save produce.

## Temp file

File managers need a file that exists on disk before `exec()` returns. It is
written under a session-scoped `QTemporaryDir` owned by `MainWindow`, with a
**fresh subdirectory per drag** so a target that reads the file lazily (or a
macOS file promise) cannot trip over the next drag:

    /tmp/enquber-XXXXXX/1/example.com-path.png
    /tmp/enquber-XXXXXX/2/example.com-path.png

The pretty basename comes from the existing `suggestedFileName()`, so the file
that lands on the desktop is named after the link rather than `qr.png`. The
whole temp directory is removed when the window is destroyed; individual files
are never deleted mid-session. If the PNG cannot be written the drag is not
started and the status line reports it. A temporary directory that cannot be
created at all (a broken `TMPDIR`, a sandbox that refuses it) is a failure too,
rather than quietly writing the PNG next to the working directory.

## Drag action: Copy only

`drag->exec(Qt::CopyAction)`. A generated symbol is *copied*, never moved:

- `MoveAction` would ask the source to delete its original and would let a
  target remove the temp file. It is only harmless here because the artifact is
  regenerable, so there is no benefit.
- `LinkAction` would make a file manager create a symlink into the temp
  directory, which silently breaks when the session ends. It is not even
  representable on Wayland.
- `CopyAction` matches dragging an image out of a browser, which is the mental
  model users already have. It is the least surprising and has no broken-link or
  destructive edge cases.

Note for the future: `QDrag::exec(actions)` picks the proposed action in the
order Move, Copy, Link, so if `MoveAction` is ever added it must be with an
explicit default — `exec(Qt::CopyAction | Qt::MoveAction, Qt::CopyAction)`.

## Not breaking the drop *target*

While our own code is dragged, moving back over the Enquber window fires
`MainWindow::dragEnterEvent` with our payload. Because `mime::textForQr()`
prefers URLs, the window would offer to "replace the current code" with the
temporary file path. The drop handlers therefore ignore drags that originate
inside this window:

```cpp
QWidget *source = qobject_cast<QWidget *>(event->source());
if (source && isAncestorOf(source)) {
    return;
}
```

External drags have a null source. A drag from another process in the same
process (the smoke test's `dragsource --with-app`) has a source widget that is
*not* a child of this window, so it is still handled.

## Decisions (and why)

- **Plain text is the file URL, not the encoded link.** The user already has the
  link in front of them before dragging; Qt's default keeps the code simple.
- **File name comes from `suggestedFileName()`.** It already sanitises to
  `[A-Za-z0-9._-]`, drops the query and caps the stem at 60 characters, so names
  are usable. *Known follow-up:* opaque path segments and long IDs still survive
  into the name (e.g. `.../d/AbC123…/view`); see `doc/improvements.txt`.
- **No `text/html`.** A rich-text target would insert
  `<img src="file:///tmp/…">` — a link that breaks when the session ends —
  instead of embedding the bytes.
- **Copy only.** See above.
- **No UI hints.** Just the open-hand cursor.

## Renaming after the drop

There is no drag-and-drop message, on any platform, that asks the target to open
its rename editor or to preselect the suggested name. Once the drop is
delivered, naming is the target's business:

- Most file managers copy the file silently under the basename Enquber provides
  and simply select the new item; F2 then preselects the stem, so renaming is
  one keystroke, but it is never automatic.
- KDE's `KIO::DropJob` *will* ask for a file name when the payload is data
  rather than URLs ("saved into a file after asking the user to choose a
  filename and the preferred data format"). Enquber also offers
  `text/uri-list` so that non-KDE managers can save a file, and with a URL
  present KIO copies silently. Dropping the URL to get the prompt would break
  Nautilus, Thunar and the desktop.
- XDS (`XdndDirectSave0`) is the one legacy protocol built around the target
  choosing the name. It is X11-only, Qt has no support for it, and modern file
  managers implement it inconsistently.
- macOS file promises and Windows OLE likewise leave the final name to the
  destination, with no "start editing" request.

So the suggested name stays `suggestedFileName()`, and the improvement to make
(see `doc/improvements.txt`) is a shorter, prettier default rather than an
automatic rename.

## Keeping the drag alive after the drop

`QDrag::exec()` returns as soon as the target has taken the drop, but the target
does **not** have the data yet: it fetches `text/uri-list` / `image/png` from the
source afterwards, over the `XdndSelection`. Destroying the `QDrag` (and with it
the selection ownership) at that point makes a strict target receive an empty
payload — the dropped URI shows up while hovering and is gone at drop time, so a
file manager saves nothing.

`startCodeDrag()` therefore keeps the finished drag alive for a moment:

```cpp
const Qt::DropAction action = drag->exec(Qt::CopyAction);

// exec() returns as soon as the drop is delivered, but the target fetches
// the payload (text/uri-list, image/png) from us afterwards, over the X11
// selection. Dropping the drag here would drop that selection with it and
// the target would receive an empty payload, so it is kept alive briefly
// and then deleted once the transfer has certainly finished.
QTimer::singleShot(kDragLingerMs, drag, [drag] { drag->deleteLater(); });
```

`kDragLingerMs` is 1000 ms. This is the same X11 on-demand-payload behaviour
that `mime::Payload` exists to work around in the other direction (see
`src/mimetext.h`); there the fix is on the target side, here it is on the
source side. Some targets (Firefox, Thunar, Konqueror) are relaxed enough to win
the race anyway, which is why the bug is easy to miss.

## Testing

**Unit (`tests/tst_enquber.cpp`, offscreen):**

- `mime::payloadForDrag()` offers an image, an explicit `image/png`, and one
  local `text/uri-list` URL pointing at an existing valid PNG.
- `QrView` emits `dragRequested` after a press plus a move past the threshold,
  and does not emit below the threshold or without a code.

`QDrag::exec()` cannot run offscreen (it blocks and needs a real drag), so the
payload and the gesture are tested separately.

**Smoke (`tools/smoke.py`, real X11):** the drag-out step drags from the code
onto `tools/droptarget.py`, a small Qt target that copies the file it is
offered like a file manager would. It asserts the target saved the file under
its URL-derived name and that `zbarimg` decodes it to the expected link. The
target is the mirror of `tools/dragsource.cpp`, which drives drops *into* the
window.

## Risks / platform notes

- **Sandboxed targets (Flatpak/Snap) may not see `/tmp`.** Qt does not integrate
  with the portal file-transfer protocol automatically. Ecosystem limitation,
  not specific to Enquber.
- **Documents may link the temp file instead of embedding it.** Offering
  `image/png` biases toward embedding; the file survives until the app exits.
  Standard trade-off for image drags.
- **The drag now outlives `exec()` by a second.** A target that takes longer
  than that to fetch the payload would still fail; the linger is a pragmatic
  bound, not a guarantee.
- **Windows / macOS / Wayland** use the same API and `setUrls` maps to CF_HDROP
  / file promises, but there is no CI for those platforms; manual testing is
  still needed.
