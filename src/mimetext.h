#pragma once

#include <QString>

class QMimeData;

namespace mime {

/// Extracts the text worth encoding from a drop or a clipboard payload.
///
/// URLs win over plain text because that is what browsers and file managers
/// put on the drag; the text is trimmed so that a copied line never carries
/// stray whitespace into the symbol. Returns an empty string when the payload
/// holds nothing that can be encoded.
QString textForQr(const QMimeData *data);

/// Remembers what a drag offered while it was still hovering.
///
/// On X11 the payload is fetched from the drag source on demand, and a source
/// is free to refuse the fetch once the button has been released: Qt's own
/// sources only answer a request whose timestamp matches the drop they
/// recorded, so dropping between two Qt applications reliably hands the target
/// an empty payload. The text cannot change during a drag, so keeping what was
/// readable while the drag hovered makes the drop work either way.
class Payload
{
public:
    /// Resolves @p data and remembers it. Call while a drag is hovering.
    /// Returns the text, or an empty string when there is nothing to encode.
    QString observe(const QMimeData *data);

    /// The text to encode: what the event itself carries, or, when the source
    /// would not hand it over again, what was observed while hovering.
    QString resolve(const QMimeData *data) const;

    /// Forgets the observation; call when the drag is over.
    void forget() { m_observed.clear(); }

private:
    QString m_observed;
};

} // namespace mime
