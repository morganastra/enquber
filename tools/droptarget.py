#!/usr/bin/env python3
"""A drop target used by the GUI smoke test.

It shows a window that accepts a drag, and copies the first local file it is
offered into --save. That is what a file manager does with a `text/uri-list`
drop, so a drag out of the application under test ends up as a real file the
smoke test can decode again.

It is the mirror of tools/dragsource.cpp, which is the source used to test
drops *into* the application. It is written with PyQt6 rather than against
XDND by hand so that it requests the payload exactly the way a real
application does; a hand written XDND target asks for the selection in a way
Qt's drag source does not always answer, which says nothing about what the
application under test actually sent.

    tools/droptarget.py --save /tmp/received

It copies one file and exits, so the smoke test can wait for it; --keep is
for driving it by hand and dropping onto it repeatedly. It prints the formats
and URLs it is offered. Exit status is 0 once a file has been saved.
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys

from PyQt6.QtCore import Qt, QTimer
from PyQt6.QtWidgets import QApplication, QLabel


def report(message: str) -> None:
    print(message, flush=True)


class DropTarget(QLabel):
    def __init__(self, save_dir: str, keep: bool):
        super().__init__(alignment=Qt.AlignmentFlag.AlignCenter)
        self.save_dir = save_dir
        self.keep = keep
        self.setText("drop a file here")
        self.setAcceptDrops(True)
        self.setWindowTitle("drop-target")
        self.resize(320, 200)
        self.setStyleSheet(
            "QLabel { background: palette(base); border: 2px solid palette(highlight); }")

    def dragEnterEvent(self, event):
        mime = event.mimeData()
        report(f"drag enter, formats: {', '.join(mime.formats())}")
        for url in mime.urls():
            report(f"  url: {url.toString()}")
        # A file manager accepts anything that names a file, and only then.
        if mime.hasUrls():
            event.acceptProposedAction()

    def dragMoveEvent(self, event):
        if event.mimeData().hasUrls():
            event.acceptProposedAction()

    def dropEvent(self, event):
        mime = event.mimeData()
        for url in mime.urls():
            source = url.toLocalFile()
            if not source or not os.path.isfile(source):
                report(f"  skipping {source!r}, it is not a local file")
                continue
            destination = os.path.join(self.save_dir, os.path.basename(source))
            try:
                shutil.copy2(source, destination)
            except OSError as error:
                report(f"  could not copy {source}: {error}")
                continue
            report(f"  copied {source} -> {destination}")
            event.acceptProposedAction()
            if not self.keep:
                QTimer.singleShot(0, QApplication.instance().quit)
            return
        report("  the drop carried no local file")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--save", required=True, help="directory to copy the received file into")
    parser.add_argument("--keep", action="store_true", help="stay up for more drops")
    parser.add_argument("--timeout", type=float, default=30.0,
                        help="give up after this many seconds with no usable drop")
    args = parser.parse_args(argv)

    os.makedirs(args.save, exist_ok=True)

    app = QApplication(sys.argv)
    target = DropTarget(args.save, args.keep)
    target.show()
    # A target that is never offered anything must not hang the test forever.
    QTimer.singleShot(int(args.timeout * 1000), lambda: app.exit(1))
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
