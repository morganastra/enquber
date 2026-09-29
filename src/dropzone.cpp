#include "dropzone.h"

#include "mimetext.h"
#include "theme.h"

#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QLabel>
#include <QPainter>
#include <QVBoxLayout>

namespace {

constexpr int kCornerRadius = 12;
constexpr qreal kBorderWidth = 2.0;
constexpr int kIconSize = 44;

QFont titleFont(const QFont &base)
{
    QFont font = base;
    font.setPointSizeF(base.pointSizeF() * 1.15);
    font.setWeight(QFont::DemiBold);
    return font;
}

} // namespace

DropZone::DropZone(QWidget *parent)
    : QWidget(parent)
{
    setAcceptDrops(true);
    setFixedSize(440, 240);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    m_icon = new QLabel(this);
    m_icon->setAlignment(Qt::AlignCenter);

    m_title = new QLabel(tr("Drop a link here"), this);
    m_title->setAlignment(Qt::AlignCenter);
    m_title->setFont(titleFont(font()));

    m_hint = new QLabel(tr("or press Ctrl+V to paste one"), this);
    m_hint->setAlignment(Qt::AlignCenter);
    QPalette hintPalette = m_hint->palette();
    hintPalette.setColor(QPalette::WindowText, hintPalette.color(QPalette::PlaceholderText));
    m_hint->setPalette(hintPalette);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(10);
    layout->addStretch(1);
    layout->addWidget(m_icon);
    layout->addWidget(m_title);
    layout->addWidget(m_hint);
    layout->addStretch(1);

    refreshIcon();
}

void DropZone::setActive(bool active)
{
    if (m_active == active) {
        return;
    }
    m_active = active;
    update();
}

void DropZone::refreshIcon()
{
    const QIcon icon = theme::icon({"insert-link", "edit-paste", "document-open"});
    m_icon->setPixmap(icon.pixmap(QSize(kIconSize, kIconSize), devicePixelRatioF()));
}

void DropZone::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::FontChange) {
        refreshIcon();
    }
}

void DropZone::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF frame = QRectF(rect()).adjusted(kBorderWidth / 2, kBorderWidth / 2, -kBorderWidth / 2, -kBorderWidth / 2);

    QColor background = palette().color(QPalette::Base);
    background.setAlphaF(m_active ? 0.55 : 0.25);
    painter.setPen(Qt::NoPen);
    painter.setBrush(background);
    painter.drawRoundedRect(frame, kCornerRadius, kCornerRadius);

    QColor border = m_active ? palette().color(QPalette::Highlight) : palette().color(QPalette::Mid);
    QPen pen(border, kBorderWidth, Qt::DashLine, Qt::RoundCap);
    pen.setDashPattern({5.0, 4.0});
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(frame, kCornerRadius, kCornerRadius);
}

void DropZone::dragEnterEvent(QDragEnterEvent *event)
{
    if (m_payload.observe(event->mimeData()).isEmpty()) {
        return;
    }
    event->acceptProposedAction();
    setActive(true);
}

void DropZone::dragMoveEvent(QDragMoveEvent *event)
{
    if (!m_payload.observe(event->mimeData()).isEmpty()) {
        event->acceptProposedAction();
    }
}

void DropZone::dragLeaveEvent(QDragLeaveEvent *event)
{
    QWidget::dragLeaveEvent(event);
    m_payload.forget();
    setActive(false);
}

void DropZone::dropEvent(QDropEvent *event)
{
    setActive(false);

    const QString text = m_payload.resolve(event->mimeData());
    m_payload.forget();
    if (text.isEmpty()) {
        return;
    }
    event->acceptProposedAction();
    Q_EMIT textDropped(text);
}
