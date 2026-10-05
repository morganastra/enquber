#include "qrcode.h"

#include <QCoreApplication>
#include <QRgb>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace qr {
namespace {

QRecLevel toQRecLevel(ErrorCorrection level)
{
    switch (level) {
    case ErrorCorrection::Low:
        return QR_ECLEVEL_L;
    case ErrorCorrection::Medium:
        return QR_ECLEVEL_M;
    case ErrorCorrection::High:
        return QR_ECLEVEL_H;
    case ErrorCorrection::Quartile:
        break;
    }
    return QR_ECLEVEL_Q;
}

QString errorForErrno(int code)
{
    switch (code) {
    case ERANGE:
        //@ QrCode
        //% "Too much data for a single QR code"
        return qtTrId("qrcode.error.too-much-data");
    case ENOMEM:
        //@ QrCode
        //% "Out of memory while encoding"
        return qtTrId("qrcode.error.out-of-memory");
    default:
        // OS-provided text, deliberately not translated.
        return QString::fromLocal8Bit(std::strerror(code));
    }
}

} // namespace

void Code::Deleter::operator()(QRcode *code) const
{
    QRcode_free(code);
}

Code Code::encode(const QString &text, ErrorCorrection level)
{
    Code code;
    code.m_text = text;
    if (text.isEmpty()) {
        //@ QrCode
        //% "Nothing to encode"
        code.m_error = qtTrId("qrcode.error.nothing");
        return code;
    }

    // libqrencode is handed UTF-8 bytes; version 0 lets it pick the smallest
    // symbol that fits.
    const QByteArray utf8 = text.toUtf8();
    errno = 0;
    QRcode *raw = QRcode_encodeString(utf8.constData(), 0, toQRecLevel(level), QR_MODE_8, 1);
    if (!raw) {
        code.m_error = errorForErrno(errno);
        return code;
    }
    code.m_code = std::shared_ptr<QRcode>(raw, Deleter {});
    return code;
}

int Code::modules() const
{
    return m_code ? m_code->width : 0;
}

bool Code::isDark(int x, int y) const
{
    if (!m_code || x < 0 || y < 0 || x >= m_code->width || y >= m_code->width) {
        return false;
    }
    return (m_code->data[y * m_code->width + x] & 1) != 0;
}

QImage Code::toImage(int modulePixels, int quietZone) const
{
    if (!isValid() || modulePixels < 1 || quietZone < 0) {
        return {};
    }

    const int width = modules();
    const int side = (width + 2 * quietZone) * modulePixels;
    QImage image(side, side, QImage::Format_RGB32);
    image.fill(Qt::white);

    // Filled rather than blitted: keeping the pixels square is what makes the
    // result crisp at any scale.
    const QRgb black = qRgb(0, 0, 0);
    for (int y = 0; y < width; ++y) {
        const int firstRow = (y + quietZone) * modulePixels;
        for (int x = 0; x < width; ++x) {
            if (!isDark(x, y)) {
                continue;
            }
            const int firstColumn = (x + quietZone) * modulePixels;
            for (int dy = 0; dy < modulePixels; ++dy) {
                QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(firstRow + dy));
                std::fill_n(line + firstColumn, modulePixels, black);
            }
        }
    }
    return image;
}

} // namespace qr
