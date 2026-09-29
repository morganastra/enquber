#include "theme.h"

#include <QIcon>

namespace theme {

QIcon icon(std::initializer_list<const char *> names)
{
    for (const char *name : names) {
        const QIcon candidate = QIcon::fromTheme(QString::fromLatin1(name));
        if (!candidate.isNull()) {
            return candidate;
        }
    }
    return {};
}

} // namespace theme
