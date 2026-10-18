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
