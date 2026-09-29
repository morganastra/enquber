#!/usr/bin/env python3
"""Drive X11 windows from a script: the test rig for the GUI.

python-xlib supplies both the window queries and XTEST for synthetic input, and
ImageMagick's `import` takes the screenshots. Both are part of a normal KDE
install, so this stays dependency free.

Used as a library by tools/smoke.py, and on the command line for poking at a
running application:

    xui.py list
    xui.py find --pid 1234
    xui.py shot <window> /tmp/shot.png
    xui.py key ctrl+v
    xui.py type "https://example.com"
    xui.py click-at <window> 280 350
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import time
from dataclasses import dataclass

from Xlib import X, XK, display
from Xlib.ext import xtest

# X11 atoms we care about.
_NET_WM_NAME = "UTF8_STRING"
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
    window_type: str

    def __str__(self) -> str:
        return (
            f"0x{self.id:08x} {self.width:5d}x{self.height:<5d} +{self.x:<5d}+{self.y:<5d} "
            f"{self.wm_class or '-':<20} pid={(self.pid if self.pid else '-'):<7} {self.title!r}"
        )


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
        except Exception:
            return None
        return prop.value if prop else None

    def _describe(self, window) -> Window | None:
        try:
            attrs = window.get_attributes()
            if attrs.map_state != X.IsViewable:
                return None
            geom = window.get_geometry()
            if geom.width <= 1 or geom.height <= 1:
                return None
        except Exception:
            return None

        title = self._property(window, "_NET_WM_NAME", utf8=True)
        if title:
            title = title.decode("utf-8", "replace") if isinstance(title, bytes) else str(title)
        else:
            title = window.get_wm_name() or ""

        wm_class = ""
        try:
            klass = window.get_wm_class()
            if klass:
                wm_class = klass[1] if len(klass) > 1 else klass[0]
        except Exception:
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
            x, y = coords.x + geom.border_width, coords.y + geom.border_width
        except Exception:
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
            window_type=window_type,
        )

    def _top_level_windows(self):
        """Yields the top level client windows.

        A window manager reparents clients into decoration frames, so the
        clients are not the direct children of the root. _NET_CLIENT_LIST is the
        reliable answer; without a window manager the root's children are the
        clients themselves.

        Excludes substructure redirect guards and other helper windows.
        """
        clients = self._property(self.root, "_NET_CLIENT_LIST")
        if clients:
            for window_id in clients:
                yield self.d.create_resource_object("window", window_id)
            return
        for child in self.root.query_tree().children:
            yield child

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
        candidates.sort(key=lambda w: (w.wm_class.lower() != (pattern or "").lower(), -w.width * w.height))
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

    def window(self, window_id: int) -> Window:
        described = self._describe(self.d.create_resource_object("window", window_id))
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
        window = self.d.create_resource_object("window", window_id)
        # source indication 2 means "pager", which window managers always honour.
        self._client_message(window, "_NET_ACTIVE_WINDOW", [2, X.CurrentTime, 0])
        try:
            window.configure(stack_mode=X.Above)
            window.set_input_focus(X.RevertToParent, X.CurrentTime)
        except Exception:
            pass
        self.d.sync()

    def move_resize(self, window_id: int, x: int, y: int, width: int, height: int):
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

    def focus(self, window_id: int):
        """Gives the window the input focus and makes it the active one.

        Qt only treats a window as active once it has the focus, and shortcuts
        are scoped to the active window. With a window manager that is its job,
        without one we have to hand out the focus ourselves.
        """
        if self._has_window_manager():
            self.activate(window_id)
            return
        window = self.d.create_resource_object("window", window_id)
        window.set_input_focus(X.RevertToParent, X.CurrentTime)
        self.d.sync()
        time.sleep(0.1)

    def raise_window(self, window_id: int):
        window = self.d.create_resource_object("window", window_id)
        window.configure(stack_mode=X.Above)
        self.d.sync()

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

    def _keycode(self, keysym: int) -> tuple[int, int] | None:
        keycode = self.d.keysym_to_keycode(keysym)
        if not keycode:
            return None
        for level in range(4):
            if self.d.keycode_to_keysym(keycode, level) == keysym:
                return keycode, level
        return keycode, 0

    def _tap(self, keysym: int, settle: float = 0.02):
        found = self._keycode(keysym)
        if found is None:
            raise ValueError(f"no keycode for keysym {keysym!r}")
        keycode, level = found
        modifiers = []
        if level & 1:
            modifiers.append(self._keycode(XK.string_to_keysym("Shift_L"))[0])
        if level & 2:
            modifiers.append(self._keycode(XK.string_to_keysym("ISO_Level3_Shift"))[0])
        for modifier in modifiers:
            xtest.fake_input(self.d, X.KeyPress, modifier)
        xtest.fake_input(self.d, X.KeyPress, keycode)
        xtest.fake_input(self.d, X.KeyRelease, keycode)
        for modifier in reversed(modifiers):
            xtest.fake_input(self.d, X.KeyRelease, modifier)
        self.d.sync()
        time.sleep(settle)

    def key(self, name: str, settle: float = 0.02):
        """Press a single named key, e.g. Return, Escape, F5."""
        keysym = XK.string_to_keysym(name)
        if keysym == 0 and len(name) == 1:
            keysym = ord(name)
        if keysym == 0:
            raise ValueError(f"unknown key {name!r}")
        self._tap(keysym, settle)

    def shortcut(self, combo: str, settle: float = 0.05):
        """Press a combination such as 'ctrl+v' or 'ctrl+shift+s'."""
        parts = [part.strip() for part in combo.split("+") if part.strip()]
        if not parts:
            raise ValueError("empty shortcut")
        modifiers = []
        for part in parts[:-1]:
            name = _MODIFIERS.get(part.lower(), part)
            found = self._keycode(XK.string_to_keysym(name))
            if found is None:
                raise ValueError(f"unknown modifier {part!r}")
            modifiers.append(found[0])
        key_part = parts[-1]
        keysym = XK.string_to_keysym(key_part)
        if keysym == 0 and len(key_part) == 1:
            keysym = ord(key_part)
        found = self._keycode(keysym)
        if found is None:
            raise ValueError(f"unknown key {key_part!r}")
        keycode, level = found
        if level & 1:
            modifiers.append(self._keycode(XK.string_to_keysym("Shift_L"))[0])

        for modifier in modifiers:
            xtest.fake_input(self.d, X.KeyPress, modifier)
        xtest.fake_input(self.d, X.KeyPress, keycode)
        xtest.fake_input(self.d, X.KeyRelease, keycode)
        for modifier in reversed(modifiers):
            xtest.fake_input(self.d, X.KeyRelease, modifier)
        self.d.sync()
        time.sleep(settle)

    def type_text(self, text: str, settle: float = 0.015):
        """Type a string one character at a time."""
        for char in text:
            keysym = ord(char) if ord(char) < 0x100 else 0
            if keysym < 0x20 or keysym > 0x7E:
                raise ValueError(f"cannot type character {char!r}")
            self._tap(keysym, settle)

    # -- screenshots ------------------------------------------------------

    def screenshot(self, path: str, window: Window | None = None, region: tuple[int, int, int, int] | None = None):
        """Saves a PNG of the whole screen, one window, or a screen region."""
        args = ["import", "-display", self.display_name, "-window", "root"]
        if window is not None:
            args += ["-crop", f"{window.width}x{window.height}+{window.x}+{window.y}"]
        elif region is not None:
            x, y, width, height = region
            args += ["-crop", f"{width}x{height}+{x}+{y}"]
        args += ["+repage", path]
        subprocess.run(args, check=True, capture_output=True)
        return path

    # -- clipboard --------------------------------------------------------

    def _xclip(self, args: list[str], data: bytes | None = None,
               capture: bool = True) -> subprocess.CompletedProcess:
        environment = dict(os.environ, DISPLAY=self.display_name)
        if capture:
            return subprocess.run(["xclip", *args], input=data,
                                  capture_output=True, env=environment)
        # xclip forks a daemon that owns the selection and inherits our pipes,
        # so anything that keeps one alive would hang waiting for EOF.
        return subprocess.run(["xclip", *args], input=data, env=environment,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def clipboard_targets(self) -> list[str]:
        result = self._xclip(["-selection", "clipboard", "-o", "-t", "TARGETS"])
        return result.stdout.decode(errors="replace").split()

    def clipboard_text(self) -> str:
        return self._xclip(["-selection", "clipboard", "-o"]).stdout.decode(errors="replace")

    def clipboard_image(self, path: str) -> bool:
        result = self._xclip(["-selection", "clipboard", "-o", "-t", "image/png"])
        if result.returncode != 0 or not result.stdout:
            return False
        with open(path, "wb") as handle:
            handle.write(result.stdout)
        return True

    def set_clipboard_text(self, text: str):
        self._xclip(["-selection", "clipboard", "-i"], text.encode(), capture=False)


# -- command line ---------------------------------------------------------


def _window_id(value: str) -> int:
    return int(value, 0)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--display", default=os.environ.get("DISPLAY", ":0"))
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("list", help="list top level windows")

    p = sub.add_parser("find", help="print the id of the best matching window")
    p.add_argument("pattern", nargs="?")
    p.add_argument("--pid", type=int)

    p = sub.add_parser("geom", help="print a window's geometry")
    p.add_argument("window", type=_window_id)

    p = sub.add_parser("activate", help="raise and focus a window")
    p.add_argument("window", type=_window_id)

    p = sub.add_parser("shot", help="screenshot a window (or the whole screen)")
    p.add_argument("window", nargs="?", type=_window_id)
    p.add_argument("path")

    p = sub.add_parser("click", help="click absolute screen coordinates")
    p.add_argument("x", type=int)
    p.add_argument("y", type=int)
    p.add_argument("--button", type=int, default=1)

    p = sub.add_parser("click-at", help="click window relative coordinates")
    p.add_argument("window", type=_window_id)
    p.add_argument("x", type=int)
    p.add_argument("y", type=int)

    p = sub.add_parser("move", help="move the pointer")
    p.add_argument("x", type=int)
    p.add_argument("y", type=int)

    p = sub.add_parser("key", help="press a key or shortcut, e.g. ctrl+v")
    p.add_argument("combo")

    p = sub.add_parser("type", help="type text")
    p.add_argument("text")

    p = sub.add_parser("clip-get", help="print the clipboard text")
    p = sub.add_parser("clip-set", help="set the clipboard text")
    p.add_argument("text")

    p = sub.add_parser("clip-image", help="save the clipboard image to a file")
    p.add_argument("path")

    args = parser.parse_args(argv)
    x = X11(args.display)

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
        if "+" in args.combo:
            x.shortcut(args.combo)
        else:
            x.key(args.combo)
    elif args.command == "type":
        x.type_text(args.text)
    elif args.command == "clip-get":
        print(x.clipboard_text(), end="")
    elif args.command == "clip-set":
        x.set_clipboard_text(args.text)
    elif args.command == "clip-image":
        if not x.clipboard_image(args.path):
            print("no image on the clipboard", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
