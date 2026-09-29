#include "mimetext.h"

#include <QMimeData>
#include <QUrl>

namespace mime {

QString textForQr(const QMimeData *data)
{
    if (!data) {
        return {};
    }

    if (data->hasUrls()) {
        for (const QUrl &url : data->urls()) {
            if (!url.isEmpty()) {
                return url.toString();
            }
        }
    }

    if (data->hasText()) {
        return data->text().trimmed();
    }

    return {};
}

bool canEncode(const QMimeData *data)
{
    return !textForQr(data).isEmpty();
}

QString Payload::observe(const QMimeData *data)
{
    // Fetched once per drag: every fetch is a round trip to the drag source,
    // and the payload cannot change while the drag is in flight.
    if (m_observed.isEmpty()) {
        m_observed = textForQr(data);
    }
    return m_observed;
}

QString Payload::resolve(const QMimeData *data) const
{
    const QString text = textForQr(data);
    return text.isEmpty() ? m_observed : text;
}

} // namespace mime
