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

#include "mimeimage.h"

#include <QBuffer>
#include <QImage>
#include <QMimeData>
#include <QUrl>

namespace mime {

QByteArray encodePng(const QImage &image)
{
    QByteArray png;
    QBuffer buffer(&png);
    if (!buffer.open(QIODevice::WriteOnly)) {
        return {};
    }
    image.save(&buffer, "PNG");
    return png;
}

QMimeData *imagePayload(const QImage &image)
{
    auto *data = new QMimeData;
    data->setImageData(image);
    const QByteArray png = encodePng(image);
    // An empty buffer would still advertise the format, so a target that
    // prefers the explicit PNG flavor would decode nothing.
    if (!png.isEmpty()) {
        data->setData(QStringLiteral("image/png"), png);
    }
    return data;
}

QMimeData *payloadForDrag(const QImage &image, const QString &filePath)
{
    QMimeData *data = imagePayload(image);
    data->setUrls({QUrl::fromLocalFile(filePath)});
    return data;
}

} // namespace mime
