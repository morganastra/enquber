#!/usr/bin/env python3
"""End to end smoke test for enquber.

Runs the real application on a display and drives it with real X input: a drag
and drop, a paste from the clipboard, the buttons, the save dialog. Every QR
code the application produces is decoded again with zbarimg, so the test proves
that what ends up on screen, on the clipboard and on disk is scannable and says
what it should.

    tools/smoke.py --display :9

Point it at a throwaway display (Xvfb) to keep windows off your desktop.
"""

from __future__ import annotations

import argparse
import math
import os
import signal
import tempfile
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from xui import X11, Window  # noqa: E402

DROPPED_URL = "https://example.com/dragged?from=smoke-test"
PASTED_URL = "https://example.com/pasted?q=1"
FOREIGN_URL = "https://example.com/from-another-process"


class Failure(Exception):
    pass


def die_with_parent() -> None:
    """Asks the kernel to terminate this process when its parent goes away.

    Runs between fork() and exec() in the child. Covered by PR_SET_PDEATHSIG,
    so even `kill -9` on the test script cannot leave application windows on
    the desktop.
    """
    import ctypes
    try:
        # CDLL(None) is the running process, which has libc's symbols; that
        # works for any libc, unlike hard coding libc.so.6.
        libc = ctypes.CDLL(None, use_errno=True)
        libc.prctl(1, signal.SIGTERM, 0, 0, 0)  # PR_SET_PDEATHSIG
    except Exception:
        pass  # best effort: the signal handlers are the main path


def install_signal_handlers() -> None:
    """Turns termination signals into an exception.

    Python runs no `finally` for a signal, so without this a `timeout`, a
    supervisor or a plain `kill` of this script leaves the application and its
    helpers running with their windows on screen. Handling SIGINT also matters
    for a subtler reason: a shell that starts this script in the background
    gives it SIGINT ignored, and children inherit that, leaving them immune to
    Ctrl-C. A handler is reset to the default across exec, so installing one
    here makes the windows killable again.
    """
    def interrupt(signum, _frame):
        raise KeyboardInterrupt(f"signal {signum}")

    for number in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(number, interrupt)


@dataclass
class App:
    process: subprocess.Popen
    window: Window


class Smoke:
    def __init__(self, app: Path, dragsource: Path, display: str, shots: Path):
        self.app = app
        self.dragsource = dragsource
        self.x = X11(display)
        self.display = display
        self.shots = shots
        self.shots.mkdir(parents=True, exist_ok=True)
        self.processes: list[subprocess.Popen] = []
        self.steps = 0

    # -- reporting -------------------------------------------------------

    def log(self, message: str):
        print(f"     {message}", flush=True)

    def step(self, title: str):
        self.steps += 1
        print(f"\n[{self.steps}] {title}", flush=True)

    def check(self, condition: bool, message: str):
        if not condition:
            raise Failure(message)
        self.log(f"ok - {message}")

    # -- driving the application -----------------------------------------

    def launch(self, command: list[str], title: str) -> App:
        environment = dict(os.environ, DISPLAY=self.display)
        environment.pop("QT_QPA_PLATFORM", None)
        try:
            process = subprocess.Popen(command, env=environment, preexec_fn=die_with_parent,
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        except FileNotFoundError as error:
            binary = Path(command[0])
            if binary == self.app:
                hint = ("build it first:\n"
                        "    just build")
            else:
                hint = ("it is a test helper, so re-run cmake with the test tools turned on:\n"
                        "    cmake -S . -B build -G Ninja -DENQUBER_BUILD_TEST_TOOLS=ON\n"
                        "    cmake --build build\n"
                        "or use just:\n"
                        "    just smoke-test")
            raise Failure(f"{binary} does not exist; {hint}") from error
        self.processes.append(process)
        window = self.x.wait_for(pattern=title, pid=process.pid, timeout=15, title_only=True)
        self.check(window is not None, f"{Path(command[0]).name} opened a window titled {title!r}")
        time.sleep(0.5)
        window = self.x.window(window.id)
        return App(process, window)

    def place(self, window_id: int, x: int, y: int, width: int, height: int) -> Window:
        self.x.move_resize(window_id, x, y, width, height)
        time.sleep(0.4)
        return self.x.window(window_id)

    def shut_down(self) -> list[str]:
        """Stops everything this run started.

        Returns the ones that would not go, so the caller can say so instead of
        leaving the user to wonder why a window is still there.
        """
        # A second Ctrl-C must not abort the clean up.
        for number in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            signal.signal(number, signal.SIG_IGN)
        for process in self.processes:
            if process.poll() is None:
                process.terminate()
        deadline = time.monotonic() + 1.5
        while time.monotonic() < deadline:
            if all(process.poll() is not None for process in self.processes):
                break
            time.sleep(0.1)
        for process in self.processes:
            if process.poll() is None:
                process.kill()
        stubborn = []
        for process in self.processes:
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                stubborn.append(f"{' '.join(process.args)} (pid {process.pid})")
        return stubborn

    def output_so_far(self, process: subprocess.Popen, timeout: float = 2.0, until=None) -> str:
        """Reads what a still running helper has printed.

        Reads the raw pipe without blocking, because the helper keeps running
        and any buffered line would otherwise be lost. Stops early once @p until
        is satisfied, and otherwise keeps reading until the timeout runs out.
        """
        import fcntl
        descriptor = process.stdout.fileno()
        fcntl.fcntl(descriptor, fcntl.F_SETFL,
                    fcntl.fcntl(descriptor, fcntl.F_GETFL) | os.O_NONBLOCK)
        collected = ""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                chunk = os.read(descriptor, 4096).decode(errors="replace")
            except BlockingIOError:
                chunk = ""
            if chunk:
                collected += chunk
                if until is not None and until(collected):
                    break
            elif until is None and collected:
                break
            else:
                time.sleep(0.1)
        return collected

    def geometry(self, window: Window) -> Window:
        """The window's current geometry.

        A tiling window manager rearranges windows whenever a new one appears,
        so a geometry captured earlier is not worth trusting.
        """
        try:
            return self.x.window(window.id)
        except RuntimeError:
            return window

    def screenshot(self, window: Window, name: str) -> Path:
        path = self.shots / f"{self.steps:02d}-{name}.png"
        self.x.screenshot(str(path), window=self.geometry(window))
        return path

    def screenshot_screen(self, name: str) -> Path:
        path = self.shots / f"{self.steps:02d}-{name}-screen.png"
        self.x.screenshot(str(path))
        return path

    def decode(self, window: Window, name: str) -> str | None:
        """Screenshots the window and returns the text of the QR code on it."""
        return decode_png(self.screenshot(window, name))


def decode_png(path: Path) -> str | None:
    result = subprocess.run(["zbarimg", "--quiet", "--raw", str(path)],
                            capture_output=True, text=True)
    return result.stdout.strip() or None


def drag_until_dropped(smoke: Smoke, source: Window, target: Window, expected: str,
                       label: str, attempts: int = 3) -> str | None:
    """Drags from one window onto another until it shows @p expected.

    Window managers move windows around between attempts, and Qt's platform
    plugin swallows a drop now and then, so one shot would be flaky rather than
    informative.
    """
    decoded = None
    for attempt in range(1, attempts + 1):
        source_now = smoke.geometry(source)
        target_now = smoke.geometry(target)
        centre = (target_now.x + target_now.width // 2, target_now.y + target_now.height // 2)
        drag_with_mouse(smoke, source_now, centre)
        time.sleep(0.8)
        decoded = smoke.decode(target, f"{label}-{attempt}")
        smoke.log(f"{label}: attempt {attempt} shows {decoded!r}")
        if decoded == expected:
            break
    return decoded


def drag_with_mouse(smoke: Smoke, source: Window, target: tuple[int, int], release: bool = True):
    """Presses on the source window and drags to a screen position.

    The pointer walks there in small steps and lets go straight after the last
    one, the way a hand does. Jumping the whole distance in a few hops makes
    Qt's platform plugin cancel the drag instead of dropping.
    """
    x = smoke.x
    source = smoke.geometry(source)
    from_x = source.x + source.width // 2
    from_y = source.y + source.height // 2
    to_x, to_y = target
    smoke.log(f"dragging from ({from_x}, {from_y}) to ({to_x}, {to_y})")
    x.move(from_x, from_y)
    time.sleep(0.3)
    x.press(1)
    time.sleep(0.2)
    steps = max(12, int(math.hypot(to_x - from_x, to_y - from_y) / 8))
    for step in range(1, steps + 1):
        fraction = step / steps
        x.move(int(from_x + (to_x - from_x) * fraction), int(from_y + (to_y - from_y) * fraction))
        time.sleep(0.012)
    if release:
        x.release(1)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--display", default=os.environ.get("DISPLAY", ":0"))
    parser.add_argument("--app", default="build/enquber",
                        help="the application binary (default: build/enquber)")
    parser.add_argument("--dragsource", default="build/tools/dragsource",
                        help="the drag helper, built with -DENQUBER_BUILD_TEST_TOOLS=ON")
    parser.add_argument("--shots", default=str(Path(tempfile.gettempdir()) / "enquber-smoke"),
                        help="where screenshots and the saved PNG go")
    args = parser.parse_args(argv)

    for tool in ("import", "xclip", "zbarimg", "identify"):
        if not shutil.which(tool):
            print(f"missing required tool: {tool}", file=sys.stderr)
            return 2

    install_signal_handlers()
    smoke = Smoke(Path(args.app).resolve(), Path(args.dragsource).resolve(),
                  args.display, Path(args.shots))
    try:
        run(smoke)
    except Failure as failure:
        print(f"\nFAILED: {failure}", file=sys.stderr)
        print(f"screenshots in {smoke.shots}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
        return 130
    except Exception as error:  # noqa: BLE001 - a stack trace is more useful here
        import traceback
        traceback.print_exc()
        print(f"\nERROR: {type(error).__name__}: {error}", file=sys.stderr)
        return 1
    finally:
        for stubborn in smoke.shut_down():
            print(f"warning: could not stop {stubborn}", file=sys.stderr)
    print(f"\nall {smoke.steps} steps passed; screenshots in {smoke.shots}")
    return 0


def run(smoke: Smoke):  # noqa: C901 - one linear scenario, read it top to bottom
    x = smoke.x

    smoke.step("the application takes a link on the command line")
    command_line = smoke.launch([str(smoke.app), PASTED_URL], "Enquber")
    command_line.window = smoke.place(command_line.window.id, 40, 40, 560, 700)
    on_start = smoke.decode(command_line.window, "command-line")
    smoke.check(on_start == PASTED_URL, f"the link from the command line is on screen ({on_start!r})")
    command_line.process.terminate()
    command_line.process.wait(timeout=10)
    time.sleep(0.5)

    smoke.step("an empty window with nothing to decode")
    # This helper hosts an application window next to a drag source, so the drag
    # below goes through the whole QDrag, XdndAware target detection, drag move
    # and drop delivery path with real input.
    app = smoke.launch([str(smoke.dragsource), "--text", DROPPED_URL, "--with-app"], "dragsource")
    time.sleep(1.0)
    hosted = x.find("Enquber", pid=app.process.pid, title_only=True)
    smoke.check(hosted is not None, "the drag helper hosts an application window")
    window = smoke.place(hosted.id, 40, 40, 560, 700)
    smoke.check(window.width > 300 and window.height > 300,
                f"window is {window.width}x{window.height}")
    smoke.screenshot(window, "empty")
    smoke.check(smoke.decode(window, "empty") is None, "no QR code before anything arrives")
    x.set_clipboard_text("")

    smoke.step("drag a link onto the rectangle with the mouse")
    source = smoke.place(x.find("dragsource", pid=app.process.pid, title_only=True).id,
                         700, 700, 320, 160)
    dropped = None
    for attempt in range(1, 4):
        source_now = smoke.geometry(source)
        window = smoke.geometry(window)
        centre = (window.x + window.width // 2, window.y + window.height // 2)
        drag_with_mouse(smoke, source_now, centre)
        time.sleep(0.8)
        dropped = smoke.decode(window, f"dropped-{attempt}")
        smoke.log(f"in-process drag: attempt {attempt} shows {dropped!r}")
        if dropped == DROPPED_URL:
            break
    smoke.check(dropped == DROPPED_URL, f"the dropped link is on screen ({dropped!r})")
    smoke.screenshot_screen("dropped")
    output = smoke.output_so_far(app.process, timeout=4.0,
                                 until=lambda text: "app text:" in text)
    smoke.log(f"drag helper printed: {output.strip().splitlines()}")
    smoke.check("drop action: 1" in output, "the window accepted the drop with the copy action")
    smoke.check(f"app text: {DROPPED_URL}" in output, "the application encoded the dropped link")

    smoke.step("drag a link in from another process")
    foreign_drag(smoke)

    smoke.step("paste a link over the dropped one")
    x.focus(window.id)
    x.set_clipboard_text(PASTED_URL)
    x.shortcut("ctrl+v")
    time.sleep(0.7)
    pasted = smoke.decode(window, "pasted")
    smoke.check(pasted == PASTED_URL, f"the pasted link replaced the dropped one ({pasted!r})")

    smoke.step("copy the code to the clipboard")
    x.set_clipboard_text("")
    x.shortcut("ctrl+c")
    time.sleep(0.7)
    clipboard_png = smoke.shots / "clipboard.png"
    smoke.check(x.clipboard_image(str(clipboard_png)), "a PNG image is on the clipboard")
    smoke.check("image/png" in x.clipboard_targets(), "the clipboard advertises image/png")
    decoded = decode_png(clipboard_png)
    smoke.check(decoded == PASTED_URL, f"the copied PNG decodes to {decoded!r}")
    size = subprocess.run(["identify", "-format", "%wx%h", str(clipboard_png)],
                          capture_output=True, text=True).stdout.strip()
    width, height = (int(part) for part in size.split("x"))
    smoke.check(width == height and width >= 512, f"the copied PNG is {size}")

    smoke.step("press Space to activate the focused action")
    x.set_clipboard_text("")
    smoke.screenshot(window, "focused-button")
    x.key("space")
    button_png = smoke.shots / "clipboard-button.png"
    copied = False
    for _ in range(8):
        time.sleep(0.3)
        if x.clipboard_image(str(button_png)):
            copied = True
            break
    smoke.check(copied, "Space activated the focused button and copied the code "
                        f"(clipboard offers {x.clipboard_targets()[:3]})")

    smoke.step("save the code through the file dialog")
    target_png = smoke.shots / "saved-by-dialog.png"
    target_png.unlink(missing_ok=True)
    x.shortcut("ctrl+s")
    time.sleep(1.2)
    dialog = x.wait_for(pattern="Save QR Code", timeout=10, title_only=True)
    smoke.check(dialog is not None, "the save dialog opened")
    smoke.log(f"dialog: {dialog}")
    smoke.screenshot_screen("save-dialog")

    x.focus(dialog.id)
    time.sleep(0.3)
    # The Qt file dialog focuses its file name field; an absolute path plus
    # return saves right there.
    x.type_text(str(target_png))
    time.sleep(0.4)
    x.key("Return")
    time.sleep(1.2)

    smoke.check(target_png.exists(), f"{target_png.name} was written")
    decoded = decode_png(target_png)
    smoke.check(decoded == PASTED_URL, f"the saved PNG decodes to {decoded!r}")
    smoke.screenshot(window, "after-save")

    smoke.step("clear the code and drag a new link in")
    x.focus(window.id)
    x.key("Escape")
    time.sleep(0.8)
    cleared = smoke.decode(window, "cleared")
    smoke.check(cleared is None, "the window went back to the drop target")
    smoke.check(smoke.geometry(window).width > 0, "the window is still there")

    again = drag_until_dropped(smoke, source, window, DROPPED_URL, label="after clearing")
    smoke.check(again == DROPPED_URL, f"the drop target works again ({again!r})")

    smoke.step("quit with Ctrl+Q")
    quitting = smoke.launch([str(smoke.app)], "Enquber")
    quitting.window = smoke.place(quitting.window.id, 40, 40, 560, 700)
    x.focus(quitting.window.id)
    time.sleep(0.3)
    x.shortcut("ctrl+q")
    deadline = time.monotonic() + 10
    while quitting.process.poll() is None and time.monotonic() < deadline:
        time.sleep(0.1)
    code = quitting.process.poll()
    smoke.check(code is not None, "Ctrl+Q closed the application")
    smoke.log(f"the application exited with code {code}")
    smoke.check(code == 0, f"the application exited cleanly (code {code})")
    smoke.check(x.find("Enquber", pid=quitting.process.pid) is None,
                "the window is gone once the process has exited")


def foreign_drag(smoke: Smoke) -> None:
    """Drags a link from a second process onto a fresh instance.

    This is the drag the application exists for, so it is fully asserted: the
    window has to end up showing the link that was dragged in.
    """
    x = smoke.x

    # A fresh instance, so that the foreign drag finds an empty window.
    target = smoke.launch([str(smoke.app)], "Enquber")
    target.window = smoke.place(target.window.id, 40, 40, 560, 700)
    x.focus(target.window.id)
    smoke.screenshot(target.window, "empty")
    smoke.log(f"empty window at {target.window}")

    # One source window, dragged from repeatedly. Opening a window per attempt
    # would make a tiling window manager rearrange the screen in between, and
    # Qt's platform plugin silently swallows the odd drop, so a single attempt
    # would not be a fair test of the application.
    foreign = smoke.launch([str(smoke.dragsource), "--text", FOREIGN_URL, "--keep"], "dragsource")
    source = smoke.place(foreign.window.id, 700, 700, 320, 160)

    # First a hover without releasing, for the record, then the drop itself.
    source_now = smoke.geometry(source)
    target_now = smoke.geometry(target.window)
    drag_with_mouse(smoke, source_now,
                    (target_now.x + target_now.width // 2, target_now.y + target_now.height // 2),
                    release=False)
    time.sleep(0.6)
    smoke.screenshot(target.window, "drag-hovering")
    x.release(1)
    time.sleep(0.8)
    dropped_in = smoke.decode(target.window, "dropped-in-1")
    if dropped_in != FOREIGN_URL:
        # Qt swallows the odd drop, so try again before believing it.
        dropped_in = drag_until_dropped(smoke, source, target.window, FOREIGN_URL,
                                        label="foreign drag", attempts=5)
    smoke.check(dropped_in == FOREIGN_URL,
                f"the link dragged in from another process is on screen ({dropped_in!r})")
    smoke.screenshot(target.window, "dropped-in")

    foreign.process.terminate()
    target.process.terminate()
    target.process.wait(timeout=10)
    time.sleep(0.5)




if __name__ == "__main__":
    sys.exit(main())
