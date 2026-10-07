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
        const QList<QUrl> urls = data->urls();
        for (const QUrl &url : urls) {
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

QString ObservedText::observe(const QMimeData *data)
{
    // Fetched once per drag: every fetch is a round trip to the drag source,
    // and the payload cannot change while the drag is in flight.
    if (m_observed.isEmpty()) {
        m_observed = textForQr(data);
    }
    return m_observed;
}

QString ObservedText::resolve(const QMimeData *data) const
{
    const QString text = textForQr(data);
    return text.isEmpty() ? m_observed : text;
}

} // namespace mime
