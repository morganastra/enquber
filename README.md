# enquber

Enquber is a simple cross-platform QR code maker application with a nice UX and near-instant startup.

Drop, paste, or type text into the window to generate a QR code; then save, copy, or drag the resulting PNG into another application.

![Enquber UI: the drop target and QR code view](doc/image/enquber-ui.png)

## Building

To build you need a functional qt6 development environment and `libqrencode`.

The `just` job runner is also highly recommended.

On Arch Linux, that would be the following packages:

    base-devel cmake ninja qt6-base qt6-tools qrencode just python

On macos, use brew:

    brew install cmake ninja qtbase qrencode pkgconf just

Building the embedded translations also needs Qt's Linguist tools (`lupdate`,
`lrelease`), which Homebrew ships with the full `qt` formula rather than
`qtbase`.

To run the smoke tests you also need

    zbar imagemagick xclip python-xlib python-pyqt6 xorg-server-xvfb

To build/run:

    just build
    just run

On macos, you need to set in your env `CMAKE_PREFIX_PATH="$(brew --prefix qtbase)"`

### Cross-compiling for Windows

It is possible to cross-compile Enquber for 64-bit Windows from a
Linux host with MinGW-w64. For details and instructions see
[doc/packaging-windows.md](doc/packaging-windows.md)

## Development

To start with, a basic theory of the program:

Everything except `main()` lives in a static library (`enquber_core`).

The UI layout and behavior is defined in `MainWindow::MainWindow`.

QR code generation starts with `MainWindow::setText()` for dropped or
pasted links, and at `MainWindow::liveEncode()` for manually typed
text.

See `qr::Code::encode()` for the code generation logic itself and
`QrView` for how the codes are displayed.

Codes are re-rendered for copying, saving, or dragging out with
`MainWindow::renderForExport()`

### Debug Logging

You can log drag and drop events from the window's perspective with 
`QT_LOGGING_RULES="enquber.dnd.debug=true"`, or `qt.qpa.xdnd.debug=true`
to see Qt's xdnd messages directly.

### Linting

`just lint` runs all the linters. There are also linter-specific recipes

    just lint
    just lint-cpp
    just lint-i18n
    just lint-py
    just lint-spell

On Arch the extra tools are:

    clang clazy ruff typos

On macOS, `brew install llvm clazy ruff typos-cli` provides the same
tools (clang-tidy and run-clang-tidy come with `llvm`).

### Testing

To run unit tests:

    just test

We also have a very luxurious smoke test setup which drives automated
UI interactions against the real app.

    just smoke-test

Extra arguments are passed through to the smoke test driver; see
`just smoke-test --help` for details.

Smoke test screenshots are written to a date-time stamped directory
under `$TMPDIR/enquber-smoke`.

### Translations

Enquber uses Qt's ID-based translations. See
[translations.md](doc/translations.md).

## Acknowledgements

This would not be possible without Kentaro Fukuchi's wonderful
[libqrencode](https://github.com/fukuchi/libqrencode)

Thanks to loferris and Khalid for multiplatform testing!

Fallback icons are from [Feather Icons](https://feathericons.com) 
Copyright (c) 2013-2023 Cole Bemis and used under the
[MIT license](data/icon/actions/LICENSE)

## License

Copyright (c) 2026 Morgan Astra

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>. 

