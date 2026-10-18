// enquber - a simple cross-platform QR code maker
// Copyright (C) 2026 Morgan Astra
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include <QByteArray>
#include <QString>

class QImage;
class QMimeData;

namespace mime {

/// The PNG bytes of @p image, as they go both on the clipboard and into a drag.
QByteArray encodePng(const QImage &image);

/// Builds the MIME payload that carries @p image alone: the QImage flavor, plus
/// the image/png bytes when they could be encoded. A failed encode leaves the
/// explicit flavor out instead of advertising an empty image; the QImage flavor
/// still holds the pixels. Both the clipboard and the drag path start here, so
/// they cannot disagree about what an image payload contains.
///
/// The caller keeps ownership of the returned payload.
QMimeData *imagePayload(const QImage &image);

/// Builds the payload for dragging the generated code out of the window.
///
/// It carries the symbol as an image (so a document embeds the pixels) and as a
/// reference to the local PNG at @p filePath (so a file manager or the desktop
/// can save a copy). Plain text is deliberately left unset: Qt derives it from
/// the URL, so a text editor pastes the file's URL the way it would for any
/// dragged file. HTML is deliberately left out, because a rich text target
/// would insert an <img> pointing at the temporary file, which stops working
/// once the session ends.
///
/// The caller keeps ownership of the returned payload and hands it to a QDrag.
/// @p filePath must name an existing file.
QMimeData *payloadForDrag(const QImage &image, const QString &filePath);

} // namespace mime
