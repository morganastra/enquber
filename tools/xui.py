#!/usr/bin/env python3
"""Drive X11 windows from a script: the test rig for the GUI.

python-xlib supplies the window queries and XTEST for synthetic input,
ImageMagick's `import` takes the screenshots, and `xclip` moves the clipboard.

Used as a library by tools/smoke.py, and on the command line for poking at a
running application. A window is named by its numeric X11 id (decimal or 0x
hex): `list` prints every window, `find` looks one up by class, title or pid.

    xui.py list
    xui.py find --pid 1234
    xui.py find Enquber
    xui.py geom 0x60000b
    xui.py shot 0x60000b /tmp/shot.png
    xui.py shot /tmp/screen.png
    xui.py key ctrl+v
    xui.py type "https://example.com"
    xui.py click-at 0x60000b 280 350

--display is a global option and must come before the subcommand.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import time
from dataclasses import dataclass

try:
    from Xlib import XK, X, display
    from Xlib import error as xerror
    from Xlib.ext import xtest
except ImportError as error:
    _XLIB_ERROR: ImportError | None = error
else:
    _XLIB_ERROR = None

# X11 atoms we care about.
_UTF8 = "UTF8_STRING"

# Window types that are never interesting to click on.
_SKIP_TYPES = {
    "_NET_WM_WINDOW_TYPE_DESKTOP",
    "_NET_WM_WINDOW_TYPE_DOCK",
    "_NET_WM_WINDOW_TYPE_TOOLTIP",
    "_NET_WM_WINDOW_TYPE_NOTIFICATION",
    "_NET_WM_WINDOW_TYPE_POPUP_MENU",
    "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU",
    "_NET_WM_WINDOW_TYPE_COMBO",
    "_NET_WM_WINDOW_TYPE_DND",
    "_NET_WM_WINDOW_TYPE_SPLASH",
}

_MODIFIERS = {
    "ctrl": "Control_L",
    "control": "Control_L",
    "shift": "Shift_L",
    "alt": "Alt_L",
    "meta": "Meta_L",
    "super": "Super_L",
}


@dataclass
class Window:
    id: int
    title: str
    wm_class: str
    x: int
    y: int
    width: int
    height: int
    pid: int | None

    def __str__(self) -> str:
        return (
            f"0x{self.id:08x} {self.width:5d}x{self.height:<5d} +{self.x:<5d}+{self.y:<5d} "
            f"{self.wm_class or '-':<20} pid={(self.pid if self.pid else '-'):<7} {self.title!r}"
        )

    @property
    def center(self) -> tuple[int, int]:
        """Screen coordinates of the window's center."""
        return self.x + self.width // 2, self.y + self.height // 2


def display_is_live(display_name: str) -> bool:
    """True when an X server answers on @p display_name."""
    try:
        display.Display(display_name).close()
    except Exception:  # noqa: BLE001 - any failure means "not usable"
        return False
    return True


class X11:
    def __init__(self, display_name: str | None = None):
        self.display_name = display_name or os.environ.get("DISPLAY", ":0")
        self.d = display.Display(self.display_name)
        self.root = self.d.screen().root

    # -- window lookups ---------------------------------------------------

    def _atom(self, name: str):
        return self.d.intern_atom(name)

    def _property(self, window, name: str, utf8: bool = False):
        atom = self._atom(name)
        kind = self._atom(_UTF8) if utf8 else X.AnyPropertyType
        try:
            prop = window.get_full_property(atom, kind)
        except xerror.XError:
            return None
        return prop.value if prop else None

    def _describe(self, window) -> Window | None:
        """Describes a window, or returns None for helper windows.

        Substructure redirect guards and other helper windows (docks, tooltips,
        popups, clipboard owners) are skipped, as are unmapped and tiny windows.
        """
        try:
            attrs = window.get_attributes()
            if attrs.map_state != X.IsViewable:
                return None
            geom = window.get_geometry()
            if geom.width <= 1 or geom.height <= 1:
                return None
        except xerror.XError:
            return None

        title = self._property(window, "_NET_WM_NAME", utf8=True)
        if title:
            title = title.decode("utf-8", "replace") if isinstance(title, bytes) else str(title)
        else:
            try:
                title = window.get_wm_name() or ""
            except xerror.XError:
                return None

        wm_class = ""
        try:
            klass = window.get_wm_class()
            if klass:
                wm_class = klass[1] if len(klass) > 1 else klass[0]
        except xerror.XError:
            pass

        pid = self._property(window, "_NET_WM_PID")
        pid = int(pid[0]) if pid else None

        window_type = ""
        types = self._property(window, "_NET_WM_WINDOW_TYPE")
        if types is not None and hasattr(types, "__getitem__") and len(types) > 0:
            window_type = self.d.get_atom_name(types[0])

        if window_type in _SKIP_TYPES:
            return None
        if not wm_class and not title:
            return None
        if re.match(r"Qt Selection Owner", title):
            return None

        try:
            # Note the awkward polarity: python-xlib's translate_coords() takes
            # the *source* window as an argument, so asking the root for the
            # window's origin is the way round that yields absolute coords.
            coords = self.root.translate_coords(window, 0, 0)
            x, y = coords.x, coords.y
        except xerror.XError:
            return None

        return Window(
            id=window.id,
            title=title,
            wm_class=wm_class,
            x=x,
            y=y,
            width=geom.width,
            height=geom.height,
            pid=pid,
        )

    def _top_level_windows(self):
        """Yields the top level client windows.

        A window manager reparents clients into decoration frames, so the
        clients are not the direct children of the root. _NET_CLIENT_LIST is the
        reliable answer; without a window manager the root's children are the
        clients themselves.
        """
        clients = self._property(self.root, "_NET_CLIENT_LIST")
        if clients:
            for window_id in clients:
                yield self.d.create_resource_object("window", window_id)
            return
        yield from self.root.query_tree().children

    def windows(self) -> list[Window]:
        result = []
        for window in self._top_level_windows():
            described = self._describe(window)
            if described:
                result.append(described)
        return result

    def find(self, pattern: str | None = None, pid: int | None = None,
             title_only: bool = False) -> Window | None:
        """Best match for a WM_CLASS/title pattern or a process id.

        Exact class matches and the largest window win, so that helper windows
        (clipboard owners, splash screens) never shadow the real one. Pass
        title_only when several windows share the same WM_CLASS.
        """
        candidates = []
        for window in self.windows():
            if pid is not None and window.pid != pid:
                continue
            if pattern is not None:
                haystack = window.title if title_only else f"{window.wm_class} {window.title}"
                if not re.search(pattern, haystack, re.IGNORECASE):
                    continue
            candidates.append(window)
        if not candidates:
            return None
        candidates.sort(key=lambda w: (
            pattern is not None and re.fullmatch(pattern, w.wm_class, re.IGNORECASE) is None,
            -w.width * w.height,
        ))
        return candidates[0]

    def wait_for(self, pattern: str | None = None, pid: int | None = None,
                 timeout: float = 10.0, title_only: bool = False) -> Window | None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            found = self.find(pattern, pid, title_only)
            if found:
                return found
            time.sleep(0.1)
        return None

    def try_window(self, window_id: int) -> Window | None:
        """The window's description, or None when it is gone or not viewable."""
        return self._describe(self.d.create_resource_object("window", window_id))

    def window(self, window_id: int) -> Window:
        described = self.try_window(window_id)
        if not described:
            raise RuntimeError(f"window 0x{window_id:x} is gone or not viewable")
        return described

    # -- window management ------------------------------------------------

    def _client_message(self, window, message: str, data: list[int]):
        event = display.event.ClientMessage(
            window=window,
            client_type=self._atom(message),
            data=(32, data + [0] * (5 - len(data))),
        )
        self.root.send_event(event, event_mask=X.SubstructureRedirectMask | X.SubstructureNotifyMask)
        self.d.flush()

    def activate(self, window_id: int):
        """Raises the window, makes it active and gives it the input focus.

        Qt only treats a window as active once it has the focus, and shortcuts
        are scoped to the active window. The WM message asks for both; the
        direct configure and focus calls are what works without a WM.
        """
        self.window(window_id)
        window = self.d.create_resource_object("window", window_id)
        # source indication 2 means "pager", which window managers always honor.
        self._client_message(window, "_NET_ACTIVE_WINDOW", [2, X.CurrentTime, 0])
        window.configure(stack_mode=X.Above)
        window.set_input_focus(X.RevertToParent, X.CurrentTime)
        self.d.sync()
        time.sleep(0.1)

    def move_resize(self, window_id: int, x: int, y: int, width: int, height: int):
        self.window(window_id)
        window = self.d.create_resource_object("window", window_id)
        if self._has_window_manager():
            # StaticGravity with all four geometry hints set.
            flags = 10 | (1 << 8) | (1 << 9) | (1 << 10) | (1 << 11)
            self._client_message(window, "_NET_MOVERESIZE_WINDOW", [flags, x, y, width, height])
        else:
            # Nothing to negotiate with: ask the server directly.
            window.configure(x=x, y=y, width=width, height=height)
        self.d.sync()

    def _has_window_manager(self) -> bool:
        check = self._property(self.root, "_NET_SUPPORTING_WM_CHECK")
        return bool(check)

    # -- input ------------------------------------------------------------

    def move(self, x: int, y: int):
        xtest.fake_input(self.d, X.MotionNotify, x=int(x), y=int(y))
        self.d.sync()

    def click(self, x: int, y: int, button: int = 1, settle: float = 0.08):
        self.move(x, y)
        time.sleep(settle)
        xtest.fake_input(self.d, X.ButtonPress, button)
        self.d.sync()
        time.sleep(0.03)
        xtest.fake_input(self.d, X.ButtonRelease, button)
        self.d.sync()
        time.sleep(settle)

    def click_in(self, window: Window, rx: int, ry: int, button: int = 1, settle: float = 0.08):
        self.click(window.x + rx, window.y + ry, button=button, settle=settle)

    def press(self, button: int = 1):
        xtest.fake_input(self.d, X.ButtonPress, button)
        self.d.sync()

    def release(self, button: int = 1):
        xtest.fake_input(self.d, X.ButtonRelease, button)
        self.d.sync()

    def _keysym(self, name: str) -> int:
        """The keysym for a keysym name or a single character."""
        keysym = XK.string_to_keysym(name)
        if keysym == 0 and len(name) == 1:
            keysym = ord(name)
        if keysym == 0:
            raise ValueError(f"unknown key {name!r}")
        return keysym

    def _keycode(self, keysym: int) -> int:
        """The keycode the server maps a keysym to."""
        keycode = self.d.keysym_to_keycode(keysym)
        if not keycode:
            raise ValueError(f"no keycode for keysym {keysym!r}")
        return keycode

    def _level_modifiers(self, level: int) -> list[int]:
        """The modifier keycodes needed to reach a keyboard level."""
        if level > 3:
            raise ValueError(f"keyboard level {level} is not supported")
        modifiers = []
        if level & 1:
            modifiers.append(self._keycode(self._keysym("Shift_L")))
        if level & 2:
            # ISO_Level3_Shift; python-xlib's keysym table has no name for it.
            modifiers.append(self._keycode(0xFE03))
        return modifiers

    def _send_chord(self, keycode: int, modifiers: list[int], settle: float):
        """Presses a key with the modifiers held, then releases everything."""
        for modifier in modifiers:
            xtest.fake_input(self.d, X.KeyPress, modifier)
        xtest.fake_input(self.d, X.KeyPress, keycode)
        xtest.fake_input(self.d, X.KeyRelease, keycode)
        for modifier in reversed(modifiers):
            xtest.fake_input(self.d, X.KeyRelease, modifier)
        self.d.sync()
        time.sleep(settle)

    def _tap(self, keysym: int, settle: float = 0.02, extra: tuple[int, ...] = ()):
        keycode = self._keycode(keysym)
        levels = self.d.get_keyboard_mapping(keycode, 1)[0]
        try:
            level = levels.index(keysym)
        except ValueError:
            level = 0
        self._send_chord(keycode, self._level_modifiers(level) + list(extra), settle)

    def key(self, name: str, settle: float = 0.02):
        """Press a single named key, e.g. Return, Escape, F5."""
        self._tap(self._keysym(name), settle)

    def shortcut(self, combo: str, settle: float = 0.05):
        """Press a combination such as 'ctrl+v' or 'ctrl+shift+s'."""
        parts = [part.strip() for part in combo.split("+") if part.strip()]
        if not parts:
            raise ValueError("empty shortcut")
        modifiers = []
        for part in parts[:-1]:
            name = _MODIFIERS.get(part.lower(), part)
            modifiers.append(self._keycode(self._keysym(name)))
        self._tap(self._keysym(parts[-1]), settle, extra=tuple(modifiers))

    def type_text(self, text: str, settle: float = 0.015):
        """Type a string one character at a time."""
        for char in text:
            keysym = ord(char) if ord(char) < 0x100 else 0
            if keysym < 0x20 or keysym > 0x7E:
                raise ValueError(f"cannot type character {char!r}")
            self._tap(keysym, settle)

    # -- screenshots ------------------------------------------------------

    def screenshot(self, path: str, window: Window | None = None):
        """Saves a PNG of the whole screen or of one window."""
        args = ["import", "-display", self.display_name, "-window", "root"]
        if window is not None:
            args += ["-crop", f"{window.width}x{window.height}+{window.x}+{window.y}"]
        args += ["+repage", path]
        subprocess.run(args, check=True, capture_output=True)
        return path

    # -- clipboard --------------------------------------------------------

    def _xclip(self, args: list[str], data: bytes | None = None,
               capture: bool = True) -> subprocess.CompletedProcess:
        environment = dict(os.environ, DISPLAY=self.display_name)
        if capture:
            # xclip exits nonzero when the selection is empty, which is a
            # normal outcome.
            return subprocess.run(["xclip", *args], input=data,
                                  capture_output=True, env=environment, check=False)
        # xclip forks a daemon that owns the selection and inherits our pipes,
        # so anything that keeps one alive would hang waiting for EOF.
        return subprocess.run(["xclip", *args], input=data, env=environment,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)

    def clipboard_targets(self) -> list[str]:
        result = self._xclip(["-selection", "clipboard", "-o", "-t", "TARGETS"])
        return result.stdout.decode(errors="replace").split()

    def clipboard_text(self) -> str:
        return self._xclip(["-selection", "clipboard", "-o"]).stdout.decode(errors="replace")

    def clipboard_image(self, path: str) -> bool:
        result = self._xclip(["-selection", "clipboard", "-o", "-t", "image/png"])
        if result.returncode != 0 or not result.stdout.startswith(b"\x89PNG\r\n\x1a\n"):
            return False
        with open(path, "wb") as handle:
            handle.write(result.stdout)
        return True

    def set_clipboard_text(self, text: str):
        self._xclip(["-selection", "clipboard", "-i"], text.encode(), capture=False)


# -- command line ---------------------------------------------------------


WINDOW_HELP = "numeric X11 window id (decimal or 0x-prefixed hex)"


def _window_id(value: str) -> int:
    return int(value, 0)


def _dispatch(x: X11, args: argparse.Namespace) -> int:
    if args.command == "list":
        for window in x.windows():
            print(window)
    elif args.command == "find":
        window = x.find(args.pattern, args.pid)
        if not window:
            print("no match", file=sys.stderr)
            return 1
        print(hex(window.id))
    elif args.command == "geom":
        print(x.window(args.window))
    elif args.command == "activate":
        x.activate(args.window)
    elif args.command == "shot":
        window = x.window(args.window) if args.window is not None else None
        x.screenshot(args.path, window=window)
    elif args.command == "click":
        x.click(args.x, args.y, args.button)
    elif args.command == "click-at":
        x.click_in(x.window(args.window), args.x, args.y)
    elif args.command == "move":
        x.move(args.x, args.y)
    elif args.command == "key":
        if len([part for part in args.combo.split("+") if part.strip()]) > 1:
            x.shortcut(args.combo)
        else:
            x.key(args.combo)
    elif args.command == "type":
        x.type_text(args.text)
    elif args.command == "clip-get":
        print(x.clipboard_text(), end="")
    elif args.command == "clip-set":
        x.set_clipboard_text(args.text)
    elif args.command == "clip-image" and not x.clipboard_image(args.path):
        print("no image on the clipboard", file=sys.stderr)
        return 1
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--display", default=os.environ.get("DISPLAY", ":0"),
                        help="X display to connect to (default: $DISPLAY, else "
                             ":0); global, so it must come before the subcommand")
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("list", help="list top level windows",
                   description="Print every interesting top level window, one "
                               "per line: id, WxH, +X+Y, WM_CLASS, pid and title. "
                               "Docks, tooltips, popups, clipboard owners, "
                               "windows with neither class nor title, and windows "
                               "no bigger than a pixel are skipped.")

    p = sub.add_parser("find", help="print the id of the best matching window",
                       description="Print the id (as 0x hex) of the best window "
                                   "for PATTERN and/or --pid. PATTERN is a case-"
                                   "insensitive regex matched against WM_CLASS "
                                   "and the title; an exact class match wins, "
                                   "otherwise the largest match does. With "
                                   "neither, the largest window wins. Prints 'no "
                                   "match' to stderr and exits 1 if nothing fits.")
    p.add_argument("pattern", nargs="?",
                   help="case-insensitive regex over WM_CLASS and title")
    p.add_argument("--pid", type=int,
                   help="only consider windows owned by this process id")

    p = sub.add_parser("geom", help="print a window's geometry",
                       description="Print the same one-line description as "
                                   "`list`, for one window.")
    p.add_argument("window", type=_window_id, help=WINDOW_HELP)

    p = sub.add_parser("activate", help="raise and focus a window",
                       description="Ask the window manager to raise and focus "
                                   "WINDOW, or configure it directly if no WM is "
                                   "running.")
    p.add_argument("window", type=_window_id, help=WINDOW_HELP)

    p = sub.add_parser("shot", help="screenshot a window (or the whole screen)",
                       description="Save a PNG with ImageMagick `import`. With "
                                   "WINDOW, capture the screen region the window "
                                   "occupies (a covering window shows instead); "
                                   "without it, capture the whole screen.")
    p.add_argument("window", nargs="?", type=_window_id,
                   help="window id to capture (omit for the whole screen)")
    p.add_argument("path", help="output PNG path")

    p = sub.add_parser("click", help="click absolute screen coordinates",
                       description="Move the pointer to absolute screen X,Y and "
                                   "press and release a mouse button.")
    p.add_argument("x", type=int, help="absolute screen x pixel")
    p.add_argument("y", type=int, help="absolute screen y pixel")
    p.add_argument("--button", type=int, default=1,
                   help="mouse button: 1=left, 2=middle, 3=right (default: 1)")

    p = sub.add_parser("click-at", help="click window relative coordinates",
                       description="Click at X,Y pixels from the window's top-"
                                   "left corner; always the left button.")
    p.add_argument("window", type=_window_id, help=WINDOW_HELP)
    p.add_argument("x", type=int, help="pixels right of the window's left edge")
    p.add_argument("y", type=int, help="pixels below the window's top edge")

    p = sub.add_parser("move", help="move the pointer",
                       description="Move the pointer to absolute screen X,Y "
                                   "without pressing a button.")
    p.add_argument("x", type=int, help="absolute screen x pixel")
    p.add_argument("y", type=int, help="absolute screen y pixel")

    p = sub.add_parser("key", help="press a key or shortcut, e.g. ctrl+v",
                       description="Tap one key. A plain COMBO is a keysym name "
                                   "(Return, Escape, F5) or a one-character key. "
                                   "A COMBO with '+' is a chord: the last part is "
                                   "the key, the earlier parts are held as "
                                   "modifiers (ctrl/control, shift, alt, meta, "
                                   "super). Shift is added automatically.")
    p.add_argument("combo", help="e.g. Return, space, F5, ctrl+v, ctrl+shift+s")

    p = sub.add_parser("type", help="type text",
                       description="Type TEXT one character at a time. Only "
                                   "printable ASCII is supported; use `key "
                                   "Return` to press Enter.")
    p.add_argument("text", help="printable ASCII text to type")

    sub.add_parser("clip-get", help="print the clipboard text",
                   description="Print the CLIPBOARD selection as text, verbatim "
                               "and with no added newline. Needs xclip.")
    p = sub.add_parser("clip-set", help="set the clipboard text",
                       description="Put TEXT on the CLIPBOARD selection as UTF-8 "
                                   "text. Needs xclip.")
    p.add_argument("text", help="text to put on the clipboard")

    p = sub.add_parser("clip-image", help="save the clipboard image to a file",
                       description="If the CLIPBOARD selection offers image/png, "
                                   "write it to PATH; otherwise print 'no image "
                                   "on the clipboard' to stderr and exit 1. Needs "
                                   "xclip.")
    p.add_argument("path", help="output PNG path")

    args = parser.parse_args(argv)
    if _XLIB_ERROR is not None:
        print(f"missing required module: {_XLIB_ERROR.name} "
              "(pip install python-xlib)", file=sys.stderr)
        return 2
    try:
        x = X11(args.display)
    except xerror.DisplayConnectionError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    try:
        return _dispatch(x, args)
    except (RuntimeError, ValueError, re.error) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    except FileNotFoundError as error:
        print(f"missing required tool: {error.filename}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
