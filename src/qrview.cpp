#include "qrview.h"

#include <QApplication>
#include <QMouseEvent>
#include <QPainter>

#include <cmath>

namespace {

constexpr qreal kPadding = 12.0;
constexpr qreal kCornerRadius = 8.0;

} // namespace

QrView::QrView(QWidget *parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    // The code can be dragged out; an open hand is the usual cue for that.
    setCursor(Qt::OpenHandCursor);
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
    m_pressed = false;
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
    return {200, 200};
}

QSize QrView::sizeHint() const
{
    return {360, 360};
}

int QrView::codeSide() const
{
    if (!m_code.isValid()) {
        return 0;
    }
    // Whole modules at whole device pixels each, as paintEvent() draws them, so
    // the field matched to this lines up with the painted image rather than the
    // raw available box (which can be a few pixels wider once the module size
    // is rounded down).
    const int modules = m_code.modules() + 2 * qr::Code::QuietZone;
    return static_cast<int>(std::lround(fittedModulePixels() * modules / devicePixelRatioF()));
}

int QrView::fittedModulePixels() const
{
    const int modules = m_code.modules() + 2 * qr::Code::QuietZone;
    const qreal available = (std::max<qreal>)(1.0, (std::min)(width(), height()) - 2 * kPadding);
    return (std::max)(1, static_cast<int>(std::floor(available * devicePixelRatioF() / modules)));
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
    const int modulePixels = fittedModulePixels();

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

void QrView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && hasCode()) {
        m_pressed = true;
        m_pressPos = event->position().toPoint();
    }
    QWidget::mousePressEvent(event);
}

void QrView::mouseMoveEvent(QMouseEvent *event)
{
    // m_pressed is tracked rather than read from event->buttons() alone, so the
    // gesture can be driven by a test; requiring the held button as well keeps
    // the check honest for real input.
    if (m_pressed && (event->buttons() & Qt::LeftButton)
        && (event->position().toPoint() - m_pressPos).manhattanLength()
               >= QApplication::startDragDistance()) {
        m_pressed = false; // one drag per press
        Q_EMIT dragRequested();
    }
    QWidget::mouseMoveEvent(event);
}

void QrView::mouseReleaseEvent(QMouseEvent *event)
{
    m_pressed = false;
    QWidget::mouseReleaseEvent(event);
}
