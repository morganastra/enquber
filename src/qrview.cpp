#include "qrview.h"

#include <QPainter>
#include <QPainterPath>

#include <cmath>

namespace {

constexpr qreal kPadding = 12.0;
constexpr qreal kCornerRadius = 8.0;

} // namespace

QrView::QrView(QWidget *parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void QrView::setCode(const qr::Code &code)
{
    m_code = code;
    dropCache();
    update();
}

void QrView::clear()
{
    m_code = qr::Code();
    dropCache();
    update();
}

void QrView::setHighlighted(bool highlighted)
{
    if (m_highlighted == highlighted) {
        return;
    }
    m_highlighted = highlighted;
    update();
}

QSize QrView::minimumSizeHint() const
{
    return QSize(200, 200);
}

QSize QrView::sizeHint() const
{
    return QSize(360, 360);
}

void QrView::dropCache()
{
    m_cache = QImage();
    m_cacheModulePixels = 0;
}

void QrView::paintEvent(QPaintEvent *)
{
    if (!m_code.isValid()) {
        return;
    }

    const qreal dpr = devicePixelRatioF();
    const int modules = m_code.modules() + 2 * qr::Code::QuietZone;
    const qreal available = std::max<qreal>(1.0, std::min(width(), height()) - 2 * kPadding);
    const int modulePixels = std::max(1, static_cast<int>(std::floor(available * dpr / modules)));

    if (modulePixels != m_cacheModulePixels || m_cache.isNull()) {
        m_cache = m_code.toImage(modulePixels);
        m_cache.setDevicePixelRatio(dpr);
        m_cacheModulePixels = modulePixels;
    }

    const QSizeF target = m_cache.deviceIndependentSize();
    const QPointF topLeft((width() - target.width()) / 2.0, (height() - target.height()) / 2.0);
    const QRectF bounds(topLeft, target);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.drawImage(topLeft, m_cache);

    // A hairline keeps the white symbol from bleeding into a light background.
    QColor hairline = palette().color(QPalette::WindowText);
    hairline.setAlphaF(0.15);
    painter.setPen(QPen(hairline, 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(bounds.adjusted(-0.5, -0.5, 0.5, 0.5));

    if (m_highlighted) {
        QPen pen(palette().color(QPalette::Highlight), 2.0, Qt::DashLine, Qt::RoundCap);
        pen.setDashPattern({6.0, 4.0});
        painter.setPen(pen);
        painter.drawRoundedRect(bounds.adjusted(-6.0, -6.0, 6.0, 6.0), kCornerRadius, kCornerRadius);
    }
}
