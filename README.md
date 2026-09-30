# enquber

Enquber is a simple cross-platform QR code maker application with a nice UX and near-instant startup

Drop or paste text into the window to generate a QR code; then save or copy the resulting PNG

![Enquber UI: the drop target and QR code view](doc/image/enquber-ui.png)

## Building

To build you need a functional qt6 development environment and `libqrencode`.

The `just` job runner is also highly recommended.

On archlinux-like systems, that would be the following packages:

    base-devel cmake ninja qt6-base qrencode just

On macos, use brew:

    brew install cmake ninja qtbase qrencode pkgconf just

To run the smoke tests you also need

    zbar imagemagick xclip python-xlib xorg-server-xvfb

To build/run:

    just build
    just run

On macos, you need to set in your env `CMAKE_PREFIX_PATH="$(brew --prefix qtbase)"`

## Development

Everything except `main()` lives in a static library (`enquber_core`) 

Basic flow when a link is dropped or pasted:

1. `MainWindow::setText()` asks `qr::Code::encode()` for a symbol, which calls
   `QRcode_encodeString()` and hands back a shared, reference counted handle.
2. `QrView` picks the largest whole number of pixels per module that fits and
   rasterises the matrix once per size; `QrView::paintEvent()` just blits it.
3. The exports re-render at ~1024 px with `MainWindow::renderForExport()`.

### Debug Logging

You can log drag and drop events from the window's perspective with 
`QT_LOGGING_RULES="enquber.dnd.debug=true"`, or `qt.qpa.xdnd.debug=true`
to see Qt's xdnd messages directly.

### Testing

To run unit tests:

    just test

We also have a very luxurious smoke test setup which drives automated UI interactions against the real app.

    just smoke-test

Extra arguments are passed through to the smoke test driver; see `just smoke-test --help` for details

Smoke test screenshots are written to `$TMPDIR/enquber-smoke` 

## Acknowledgements

This would not be possible without Kentaro Fukuchi's wonderful [libqrencode](https://github.com/fukuchi/libqrencode)

Thanks to loferris and Khalid for multiplatform testing!

Fallback icons are from [Feather Icons](https://feathericons.com) 
Copyright (c) 2013-2023 Cole Bemis and used under the [MIT license](data/icon/actions/LICENSE)

## License

Copyright (c) 2026 Morgan Astra

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>. 

