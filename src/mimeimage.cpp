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

QMimeData *payloadForDrag(const QImage &image, const QString &filePath)
{
    auto *data = new QMimeData;
    data->setImageData(image);
    data->setData(QStringLiteral("image/png"), encodePng(image));
    data->setUrls({QUrl::fromLocalFile(filePath)});
    return data;
}

} // namespace mime
