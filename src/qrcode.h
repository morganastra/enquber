#pragma once

#include <QImage>
#include <QString>

#include <cstdint>
#include <memory>

#include <qrencode.h>

namespace qr {

/// How much of the symbol may be damaged and still decode.
enum class ErrorCorrection : std::uint8_t {
    Low,       ///< 7%
    Medium,    ///< 15%
    Quartile,  ///< 25% -- a good default for URLs
    High,      ///< 30%
};

/// An encoded QR symbol: the module matrix from libqrencode plus the text it
/// stands for.
///
/// The class is implicitly shared and cheap to copy. When encode() fails it
/// returns an invalid Code whose error() explains why; a default-constructed
/// Code is invalid but carries no reason.
class Code
{
public:
    /// Modules of white space that must surround the symbol.
    static constexpr int QuietZone = 4;

    Code() = default;

    /// Encodes @p text as UTF-8. Returns an invalid Code when the text is
    /// empty or does not fit into a single symbol; error() explains why.
    static Code encode(const QString &text, ErrorCorrection level = ErrorCorrection::Quartile);

    [[nodiscard]] bool isValid() const { return static_cast<bool>(m_code); }

    /// Human readable reason why the code is invalid, or an empty string.
    [[nodiscard]] QString error() const { return m_error; }

    /// The text that was encoded; may be empty for an invalid Code.
    [[nodiscard]] QString text() const { return m_text; }

    /// Width and height of the symbol in modules, quiet zone excluded.
    [[nodiscard]] int modules() const;

    /// Width and height of the symbol in modules, quiet zone included; 0 for
    /// an invalid Code.
    [[nodiscard]] int totalModules() const;

    /// The largest whole number of device pixels per module whose total side
    /// fits @p budgetPixels, but never fewer than @p minimumPixels; 0 for an
    /// invalid Code.
    [[nodiscard]] int modulePixelsFor(qreal budgetPixels, int minimumPixels = 1) const;

    /// True for modules that are drawn black.
    [[nodiscard]] bool isDark(int x, int y) const;

    /// Renders the symbol at @p modulePixels device pixels per module,
    /// including a QuietZone module wide border. Returns a null image for an
    /// invalid Code or when @p modulePixels is less than 1.
    [[nodiscard]] QImage toImage(int modulePixels) const;

private:
    struct Deleter {
        void operator()(QRcode *code) const;
    };

    std::shared_ptr<QRcode> m_code;
    QString m_text;
    QString m_error;
};

} // namespace qr
