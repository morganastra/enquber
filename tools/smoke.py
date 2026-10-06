#!/usr/bin/env python3
"""End to end smoke test for enquber.

Runs the real application on a display and drives it with real X input: a drag
and drop, a paste from the clipboard, the buttons, the save dialog. Every QR
code the application produces is decoded again with zbarimg, so the test proves
that what ends up on screen, on the clipboard and on disk is scannable and says
what it should.

    just smoke-test

It starts its own Xvfb display and stops it afterwards, so no display needs
setting up by hand and the windows never touch your desktop. The test takes
over the mouse and keyboard and overwrites the X clipboard, so running it on a
display you are using would be disruptive; pass --no-xvfb --display :N only
when you want that. Native Wayland is not supported.

Needs the smoke build tree, because the drag helper is only built there:

    cmake --preset smoke && cmake --build --preset smoke

and these on PATH: import and identify (ImageMagick), xclip, zbarimg (zbar).
The Python modules python-xlib and PyQt6 (for tools/droptarget.py) are also
required.

Exit status: 0 all steps passed, 1 a check failed or an error occurred,
2 a required tool or module is missing, 130 interrupted.
"""

from __future__ import annotations

import argparse
import math
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

try:
    from xui import X11, Window
except ImportError as error:  # xui imports python-xlib
    X11 = None  # type: ignore[assignment]
    Window = None  # type: ignore[assignment]
    _XLIB_ERROR: ImportError | None = error
else:
    _XLIB_ERROR = None

DROPPED_URL = "https://example.com/dragged?from=smoke-test"
PASTED_URL = "https://example.com/pasted?q=1"
FOREIGN_URL = "https://example.com/from-another-process"
# Typed by hand through the Ctrl+L field; deliberately plain ASCII and spaces so
# it can be entered with real key events.
MANUAL_TEXT = "manual typing example"


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
    except (OSError, AttributeError):
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


def display_is_live(display: str) -> bool:
    """True when an X server answers on @p display."""
    try:
        from Xlib import display as xdisplay
        xdisplay.Display(display).close()
    except Exception:  # noqa: BLE001 - any failure means "not usable"
        return False
    return True


class Xvfb:
    """A private Xvfb display, started on demand and stopped when done.

    Running against a throwaway server is what the test wants (it takes over
    the pointer and keyboard and rewrites the clipboard), so it is the default.
    """

    def __init__(self, geometry: str = "1280x1024x24"):
        self.geometry = geometry
        self.process: subprocess.Popen | None = None
        self.display = ""

    def start(self) -> str:
        """Starts Xvfb and returns the display it chose, e.g. ":99".

        A specific number is tried first and `-displayfd` confirms readiness;
        if that number was taken between the check and the start, the next one
        is tried, so two tests starting at once do not collide.
        """
        if not shutil.which("Xvfb"):
            raise Failure("Xvfb is not on PATH; install it, or pass "
                          "--no-xvfb --display :N to drive an existing server")
        last_error = ""
        for number in self._candidates():
            try:
                self.display = f":{number}"
                self._spawn(number)
            except Failure as failure:
                last_error = str(failure)
                self.stop()
                continue
            return self.display
        raise Failure(f"could not start Xvfb: {last_error or 'no free display'}")

    @staticmethod
    def _candidates():
        """Display numbers to try, low enough to be conventional."""
        for number in range(99, 130):
            if not Path(f"/tmp/.X{number}-lock").exists() \
                    and not Path(f"/tmp/.X11-unix/X{number}").exists():
                yield number

    def _spawn(self, number: int) -> None:
        read_fd, write_fd = os.pipe()
        try:
            self.process = subprocess.Popen(
                ["Xvfb", f":{number}", "-displayfd", str(write_fd),
                 "-screen", "0", self.geometry, "-nolisten", "tcp"],
                pass_fds=(write_fd,),
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            )
        finally:
            os.close(write_fd)

        # Xvfb writes the display number it bound, then a newline, when ready.
        import select as _select
        ready, _, _ = _select.select([read_fd], [], [], 5.0)
        reported = ""
        if ready:
            reported = os.read(read_fd, 16).decode(errors="replace").strip().lstrip(":")
        os.close(read_fd)

        if not reported or self.process.poll() is not None:
            raise Failure(f"Xvfb did not come up on :{number}")
        self.display = f":{reported}"
        if not display_is_live(self.display):
            raise Failure(f"Xvfb reported {self.display} but nothing answers there")

    def stop(self) -> None:
        if self.process is None or self.process.poll() is not None:
            return
        self.process.terminate()
        try:
            self.process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=3)


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
        # The checks below match English titles and dialog text, so pin the
        # application to the C locale instead of inheriting a translated
        # desktop from the developer running the test.
        environment.pop("LANGUAGE", None)
        environment["LC_ALL"] = "C"
        environment["LANG"] = "C"
        try:
            # PDEATHSIG needs preexec_fn; the rule's thread hazard cannot apply here.
            process = subprocess.Popen(command, env=environment, preexec_fn=die_with_parent,  # noqa: PLW1509
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

    def decode(self, window: Window, name: str) -> tuple[str | None, Path]:
        """Screenshots the window and returns its QR text and the screenshot.

        Include screenshot path in return tuple so we can print it on failure.
        """
        path = self.screenshot(window, name)
        return decode_png(path), path

    def check_decoded(self, decoded: str | None, expected: str | None,
                      message: str, path: Path) -> None:
        """Checks a decoded QR text, pointing at @p path when it does not scan."""
        if decoded != expected:
            if decoded is None and expected is not None:
                raise Failure(f"{message}: no QR code could be scanned in {path} "
                              f"(is the window hidden behind another one?)")
            if expected is None:
                raise Failure(f"{message}: expected no QR code, but {path} "
                              f"decodes to {decoded!r}")
            raise Failure(f"{message}: {path} decodes to {decoded!r}, "
                          f"not {expected!r}")
        self.log(f"ok - {message} ({decoded!r})")


def decode_png(path: Path) -> str | None:
    # zbarimg exits nonzero when the image holds no symbol, which some checks
    # expect, so the exit code is not asserted; stdout carries the result.
    result = subprocess.run(["zbarimg", "--quiet", "--raw", str(path)],
                            capture_output=True, text=True, check=False)
    return result.stdout.strip() or None


def drag_until_dropped(smoke: Smoke, source: Window, target: Window, expected: str,
                       label: str, attempts: int = 3) -> tuple[str | None, Path]:
    """Drags from one window onto another until it shows @p expected.

    Window managers move windows around between attempts, and Qt's platform
    plugin swallows a drop now and then, so one shot would be flaky rather than
    informative. Returns the last decoded text together with its screenshot.
    """
    decoded: str | None = None
    path = Path()
    for attempt in range(1, attempts + 1):
        source_now = smoke.geometry(source)
        target_now = smoke.geometry(target)
        center = (target_now.x + target_now.width // 2, target_now.y + target_now.height // 2)
        drag_with_mouse(smoke, source_now, center)
        time.sleep(0.8)
        decoded, path = smoke.decode(target, f"{label}-{attempt}")
        smoke.log(f"{label}: attempt {attempt} shows {decoded!r}")
        if decoded == expected:
            break
    return decoded, path


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
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--display", default=None,
                        help="X display to drive when --no-xvfb is given, e.g. "
                             ":9 (default: $DISPLAY, else :0)")
    parser.add_argument("--xvfb", dest="xvfb", action="store_true", default=None,
                        help="start a private Xvfb and drive that (the default, "
                             "so the test never touches your desktop)")
    parser.add_argument("--no-xvfb", dest="xvfb", action="store_false",
                        help="never start Xvfb; drive --display as given")
    parser.add_argument("--app", default="build/smoke/enquber",
                        help="the application binary to test "
                             "(default: build/smoke/enquber)")
    parser.add_argument("--dragsource", default="build/smoke/tools/dragsource",
                        help="the drag helper, built with "
                             "-DENQUBER_BUILD_TEST_TOOLS=ON "
                             "(default: build/smoke/tools/dragsource)")
    parser.add_argument("--shots", default=None,
                        help="where to write screenshots (default: a date-time "
                             "stamped directory under $TMPDIR/enquber-smoke)")
    args = parser.parse_args(argv)

    for tool in ("import", "xclip", "zbarimg", "identify"):
        if not shutil.which(tool):
            print(f"missing required tool: {tool}", file=sys.stderr)
            return 2

    if _XLIB_ERROR is not None:
        print(f"missing required module: {_XLIB_ERROR.name} "
              "(pip install python-xlib)", file=sys.stderr)
        return 2

    # The drag-out step needs a drop target, which is built on PyQt6 (see
    # tools/droptarget.py). The other steps do not, but there is no point
    # starting a run that will fail part way through.
    import importlib.util

    if importlib.util.find_spec("PyQt6") is None:
        print("missing required module: PyQt6 (needed by tools/droptarget.py)",
              file=sys.stderr)
        return 2

    if args.shots:
        shots = Path(args.shots)
    else:
        shots = (Path(tempfile.gettempdir()) / "enquber-smoke"
                 / time.strftime("%Y-%m-%d_%H-%M-%S"))

    # Xvfb is the default: the test seizes the pointer and keyboard and rewrites
    # the clipboard, so it must never run on a display the user is using. Only
    # --no-xvfb opts out, and then --display (or $DISPLAY) is driven as given.
    xvfb = Xvfb()
    use_xvfb = args.xvfb is not False
    display = args.display if args.display is not None \
        else os.environ.get("DISPLAY", ":0")
    try:
        if use_xvfb:
            display = xvfb.start()
            print(f"using Xvfb display {display} "
                  "(pass --no-xvfb to use your own)", flush=True)
    except Failure as failure:
        print(f"FAILED: {failure}", file=sys.stderr)
        return 1

    install_signal_handlers()
    if not display_is_live(display):
        print(f"FAILED: no X server on {display}", file=sys.stderr)
        xvfb.stop()
        return 1
    smoke = Smoke(Path(args.app).resolve(), Path(args.dragsource).resolve(),
                  display, shots)
    try:
        run(smoke)
    except Failure as failure:
        print(f"\nFAILED: {failure}", file=sys.stderr)
        print(f"screenshots in {smoke.shots}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
        return 130
    except Exception as error:  # noqa: BLE001 - top-level handler prints the traceback
        import traceback
        traceback.print_exc()
        print(f"\nERROR: {type(error).__name__}: {error}", file=sys.stderr)
        return 1
    finally:
        for stubborn in smoke.shut_down():
            print(f"warning: could not stop {stubborn}", file=sys.stderr)
        xvfb.stop()
    print(f"\nall {smoke.steps} steps passed; screenshots in {smoke.shots}")
    return 0


def run(smoke: Smoke):  # one linear scenario, read it top to bottom
    x = smoke.x

    smoke.step("the application takes a link on the command line")
    command_line = smoke.launch([str(smoke.app), PASTED_URL], "Enquber")
    command_line.window = smoke.place(command_line.window.id, 40, 40, 560, 700)
    on_start, on_start_shot = smoke.decode(command_line.window, "command-line")
    smoke.check_decoded(on_start, PASTED_URL,
                        "the link from the command line is on screen", on_start_shot)
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
    empty, empty_shot = smoke.decode(window, "empty")
    smoke.check_decoded(empty, None, "no QR code before anything arrives", empty_shot)
    x.set_clipboard_text("")

    smoke.step("drag a link onto the rectangle with the mouse")
    source = smoke.place(x.find("dragsource", pid=app.process.pid, title_only=True).id,
                         700, 700, 320, 160)
    dropped: str | None = None
    dropped_shot = Path()
    for attempt in range(1, 4):
        source_now = smoke.geometry(source)
        window = smoke.geometry(window)
        center = (window.x + window.width // 2, window.y + window.height // 2)
        drag_with_mouse(smoke, source_now, center)
        time.sleep(0.8)
        dropped, dropped_shot = smoke.decode(window, f"dropped-{attempt}")
        smoke.log(f"in-process drag: attempt {attempt} shows {dropped!r}")
        if dropped == DROPPED_URL:
            break
    smoke.check_decoded(dropped, DROPPED_URL, "the dropped link is on screen", dropped_shot)
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
    pasted, pasted_shot = smoke.decode(window, "pasted")
    smoke.check_decoded(pasted, PASTED_URL,
                        "the pasted link replaced the dropped one", pasted_shot)

    smoke.step("copy the code to the clipboard")
    x.set_clipboard_text("")
    x.shortcut("ctrl+c")
    time.sleep(0.7)
    clipboard_png = smoke.shots / "clipboard.png"
    smoke.check(x.clipboard_image(str(clipboard_png)), "a PNG image is on the clipboard")
    smoke.check("image/png" in x.clipboard_targets(), "the clipboard advertises image/png")
    decoded = decode_png(clipboard_png)
    smoke.check_decoded(decoded, PASTED_URL, "the copied PNG decodes", clipboard_png)
    size = subprocess.run(["identify", "-format", "%wx%h", str(clipboard_png)],
                          capture_output=True, text=True, check=True).stdout.strip()
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
    smoke.check_decoded(decoded, PASTED_URL, "the saved PNG decodes", target_png)
    smoke.screenshot(window, "after-save")

    smoke.step("drag the code out to another application")
    drag_out(smoke, window, PASTED_URL)

    smoke.step("clear the code and drag a new link in")
    x.focus(window.id)
    x.key("Escape")
    time.sleep(0.8)
    cleared, cleared_shot = smoke.decode(window, "cleared")
    smoke.check_decoded(cleared, None, "the window went back to the drop target", cleared_shot)
    smoke.check(smoke.geometry(window).width > 0, "the window is still there")

    again, again_shot = drag_until_dropped(smoke, source, window, DROPPED_URL,
                                           label="after clearing")
    smoke.check_decoded(again, DROPPED_URL, "the drop target works again", again_shot)

    smoke.step("type the text by hand with Ctrl+L")
    x.focus(window.id)
    time.sleep(0.3)
    x.shortcut("ctrl+l")
    time.sleep(0.8)
    # The field opens with the current code selected, so typing replaces it.
    smoke.screenshot(window, "type-open")
    x.type_text(MANUAL_TEXT)
    time.sleep(0.8)
    typed, typed_shot = smoke.decode(window, "typed")
    smoke.check_decoded(typed, MANUAL_TEXT, "the hand typed text is on screen", typed_shot)
    x.key("Return")
    time.sleep(0.8)
    confirmed, confirmed_shot = smoke.decode(window, "typed-confirmed")
    smoke.check_decoded(confirmed, MANUAL_TEXT,
                        "Return keeps the hand typed code", confirmed_shot)
    smoke.screenshot(window, "typed-done")

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


def drag_out(smoke: Smoke, source: Window, expected: str) -> None:
    """Drags the generated code onto a drop target and checks what arrived.

    The target is the mirror of tools/dragsource.cpp: it accepts the drag and
    copies the file it is offered, which is what a file manager does. It exits
    once it has copied something, so the test drags until it has.
    """
    received = smoke.shots / "drag-out"
    if received.exists():
        shutil.rmtree(received)

    target = smoke.launch([sys.executable,
                           str(Path(__file__).resolve().parent / "droptarget.py"),
                           "--save", str(received)], "drop-target")
    target.window = smoke.place(target.window.id, 700, 40, 360, 240)
    destination = (target.window.x + target.window.width // 2,
                   target.window.y + target.window.height // 2)

    for attempt in range(1, 7):
        drag_with_mouse(smoke, smoke.geometry(source), destination)
        time.sleep(1.0)
        # Numbered like the other retry loops, so a failure keeps the evidence
        # of every attempt instead of only the last one.
        smoke.screenshot_screen(f"drag-out-{attempt}")
        if sorted(received.glob("*.png")):
            break
    else:
        target.process.terminate()
        target.process.wait(timeout=5)
        raise Failure("the drag out was never accepted by the drop target "
                      f"(screenshots in {smoke.shots})")

    saved = sorted(received.glob("*.png"))
    smoke.check(bool(saved), "the target saved a PNG file")
    # The name itself is suggestedFileName()'s business and may change; what
    # the drag guarantees is that the file arrives named after the link.
    smoke.check(saved[0].suffix == ".png" and "example.com" in saved[0].name,
                f"the dropped file is named after the link ({saved[0].name})")
    decoded = decode_png(saved[0])
    smoke.check_decoded(decoded, expected, f"the dragged file {saved[0].name} decodes", saved[0])


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
    dropped_in, dropped_in_shot = smoke.decode(target.window, "dropped-in-1")
    if dropped_in != FOREIGN_URL:
        # Qt swallows the odd drop, so try again before believing it.
        dropped_in, dropped_in_shot = drag_until_dropped(smoke, source, target.window, FOREIGN_URL,
                                                         label="foreign drag", attempts=5)
    smoke.check_decoded(dropped_in, FOREIGN_URL,
                        "the link dragged in from another process is on screen", dropped_in_shot)
    smoke.screenshot(target.window, "dropped-in")

    foreign.process.terminate()
    target.process.terminate()
    target.process.wait(timeout=10)
    time.sleep(0.5)




if __name__ == "__main__":
    sys.exit(main())
