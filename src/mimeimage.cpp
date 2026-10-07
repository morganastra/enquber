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
