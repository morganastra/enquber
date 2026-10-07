#!/usr/bin/env python3
"""An XDND drag source written straight against the protocol.

Qt's own drag source reports success while the receiving Qt application never
sees the drop, so this sends the client messages by hand and waits for the
answers. That makes the exchange observable (and controllable), and it is what a
browser, GTK or Qt does when a link is dragged:

    XdndEnter    -> types on offer
    <- XdndStatus  whether the target accepts, and with which action
    XdndPosition -> where the pointer is
    XdndDrop     -> button released, target may take the data
    <- XdndFinished

The data itself travels over the XdndSelection, which this process owns; the
target converts it when it wants it, and the requests and replies are logged.

Get a target window id from tools/xui.py, e.g.

    id=$(tools/xui.py find Enquber)
    tools/xdnd.py --target "$id" --text https://example.com

The payload is offered as both text/uri-list and plain text/UTF8_STRING, and
the drop always uses the copy action. Without --x/--y the drop lands on the
center of the target window. This is a manual debugging tool; the automated
tests use tools/dragsource.cpp instead.

Needs python-xlib and a real X11 display (no native Wayland).

Exit status is 0 when the target accepted the drop and finished it.
"""

from __future__ import annotations

import argparse
import os
import select
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

try:
    from Xlib import X, Xatom, display
    from Xlib import error as xerror
    from Xlib.protocol import request
except ImportError as error:
    _XLIB_ERROR: ImportError | None = error
else:
    _XLIB_ERROR = None

XDND_VERSION = 5


class XdndSource:
    def __init__(self, display_name: str, payload: str):
        self.d = display.Display(display_name)
        self.root = self.d.screen().root
        self.payload = payload
        self.accepted_action = 0
        self.finished = False
        self.requests = 0
        self.window = self.root.create_window(
            0, 0, 120, 60, 0, self.d.screen().root_depth,
            X.InputOutput, X.CopyFromParent,
            override_redirect=1,
            # SelectionRequest and ClientMessage events are delivered to the
            # selection owner and to the addressed window without a mask.
            event_mask=X.PropertyChangeMask | X.StructureNotifyMask,
        )
        self.window.set_wm_name("xdnd-source")
        self.window.set_wm_class("xdnd-source", "XdndSource")
        self.atom_aware = self.d.intern_atom("XdndAware")
        self.atom_selection = self.d.intern_atom("XdndSelection")
        self.atom_typelist = self.d.intern_atom("XdndTypeList")
        self.atom_enter = self.d.intern_atom("XdndEnter")
        self.atom_position = self.d.intern_atom("XdndPosition")
        self.atom_drop = self.d.intern_atom("XdndDrop")
        self.atom_status = self.d.intern_atom("XdndStatus")
        self.atom_finished = self.d.intern_atom("XdndFinished")
        self.atom_action_copy = self.d.intern_atom("XdndActionCopy")
        self.window.change_property(self.atom_aware, Xatom.ATOM, 32, [XDND_VERSION])
        self.window.map()
        self.d.sync()

    # -- helpers ---------------------------------------------------------

    def _atom_name(self, atom: int) -> str:
        if not atom:
            return "none"
        try:
            return self.d.get_atom_name(atom)
        except xerror.XError:
            return str(atom)

    def note(self, message: str):
        print(f"  {message}", flush=True)

    def _send(self, target: int, kind: int, data: list[int]):
        event = display.event.ClientMessage(
            window=target, client_type=kind, data=(32, (data + [0] * 5)[:5]))
        target_window = self.d.create_resource_object("window", target)
        target_window.send_event(event, event_mask=0)
        self.d.flush()

    def _types(self) -> list[int]:
        names = ["text/uri-list", "text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "STRING"]
        return [self.d.intern_atom(name) for name in names]

    def _serve(self, request):
        """Answers a SelectionRequest with the payload."""
        self.requests += 1
        target_atom = request.target
        name = self.d.get_atom_name(target_atom)
        reply_property = request.property if request.property != X.NONE else target_atom
        if name == "TARGETS":
            atoms = self._types()
            data = [*atoms, self.d.intern_atom("TARGETS")]
            kind = Xatom.ATOM
            data_format = 32  # a list of atoms, not bytes
        elif name == "text/uri-list":
            data = (self.payload + "\r\n").encode()
            kind = target_atom
            data_format = 8
        else:
            data = self.payload.encode()
            kind = target_atom
            data_format = 8
        self.note(f"selection request for {name!r} -> {len(data)} items, format {data_format}")
        self.d.create_resource_object("window", request.requestor).change_property(
            reply_property, kind, data_format, data)
        self.d.sync()
        notify = display.event.SelectionNotify(
            time=request.time, requestor=request.requestor, selection=request.selection,
            target=target_atom, property=reply_property)
        self.d.create_resource_object("window", request.requestor).send_event(notify, event_mask=0)
        self.d.flush()

    def _pump(self, seconds: float):
        """Reads X events for a while, answering selection requests."""
        deadline = time.monotonic() + seconds
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return
            ready, _, _ = select.select([self.d.fileno()], [], [], remaining)
            if not ready:
                return
            while self.d.pending_events():
                event = self.d.next_event()
                if event.type == X.SelectionRequest:
                    self._serve(event)
                elif event.type == X.ClientMessage:
                    # Both messages carry the sender's window in l[0], the flags
                    # in l[1] (bit 0: accepted) and the action later on.
                    if event.client_type == self.atom_status:
                        data = event.data[1]
                        accept = bool(data[1] & 1)
                        action = self._atom_name(data[4])
                        self.note(f"target status: accept={accept} action={action}")
                        if accept:
                            self.accepted_action = data[4]
                    elif event.client_type == self.atom_finished:
                        data = event.data[1]
                        self.finished = bool(data[1] & 1)
                        self.accepted_action = data[2]
                        self.note(f"target finished: accept={self.finished} "
                                  f"action={self._atom_name(data[2])}")
                        return

    def drop_on(self, target: int, x: int, y: int) -> bool:
        atoms = self._types()
        self.window.change_property(self.atom_typelist, Xatom.ATOM, 32, atoms)
        # Not self.d: Xlib.display.Display is a wrapper around the real display,
        # and a hand built request has to be sent through that one.
        request.SetSelectionOwner(display=self.d.display, window=self.window,
                                  selection=self.atom_selection, time=X.CurrentTime)
        self.d.sync()
        owner = self.d.get_selection_owner(self.atom_selection)
        if owner.id != self.window.id:
            raise RuntimeError("could not take the XdndSelection")
        self.d.sync()

        self.note(f"enter on 0x{target:x}")
        self._send(target, self.atom_enter, [self.window.id, XDND_VERSION << 24 | 1])
        self._pump(0.3)

        self.note(f"position ({x}, {y})")
        self._send(target, self.atom_position,
                   [self.window.id, 0, (x << 16) + y, X.CurrentTime, self.atom_action_copy])
        self._pump(0.6)
        if not self.accepted_action:
            self.note("the target did not accept the position yet")
            # Keep going anyway: some targets answer late.

        self.note("drop")
        self._send(target, self.atom_drop, [self.window.id, 0, X.CurrentTime])
        self._pump(1.5)
        return self.finished


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--display", default=None,
                        help="X display to connect to (default: $DISPLAY); must "
                             "be X11, not native Wayland")
    parser.add_argument("--target", type=lambda v: int(v, 0), required=True,
                        help="window id of the drop target, decimal or 0x hex "
                             "(find one with 'tools/xui.py list' or "
                             "'tools/xui.py find PATTERN')")
    parser.add_argument("--text", required=True,
                        help="the payload to drag; offered as both text/uri-list "
                             "and plain text/UTF8_STRING")
    parser.add_argument("--x", type=int, default=None,
                        help="screen x of the drop (default: the target's "
                             "center; pass both --x and --y)")
    parser.add_argument("--y", type=int, default=None,
                        help="screen y of the drop (default: the target's "
                             "center; pass both --x and --y)")
    args = parser.parse_args(argv)

    if _XLIB_ERROR is not None:
        print(f"missing required module: {_XLIB_ERROR.name} "
              "(pip install python-xlib)", file=sys.stderr)
        return 2

    try:
        source = XdndSource(args.display, args.text)
    except (xerror.DisplayConnectionError, xerror.DisplayNameError):
        name = args.display if args.display is not None else os.environ.get("DISPLAY")
        if name:
            print(f"error: can't connect to display {name}", file=sys.stderr)
        else:
            print("error: no display: set DISPLAY", file=sys.stderr)
        return 2
    if args.x is None or args.y is None:
        geometry = source.d.create_resource_object("window", args.target).get_geometry()
        coords = source.root.translate_coords(
            source.d.create_resource_object("window", args.target), 0, 0)
        x = coords.x + geometry.width // 2
        y = coords.y + geometry.height // 2
    else:
        x, y = args.x, args.y

    print(f"dropping {args.text!r} on 0x{args.target:x} at ({x}, {y})")
    accepted = source.drop_on(args.target, x, y)
    print(f"accepted={accepted} action={source.accepted_action} requests={source.requests}")
    return 0 if accepted else 1


if __name__ == "__main__":
    sys.exit(main())
