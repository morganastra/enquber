#include "dropzone.h"

#include "theme.h"

#include <QCoreApplication>
#include <QLabel>
#include <QMouseEvent>
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
    setFixedSize(440, 240);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    // The prompt doubles as a "click here to type" affordance; an I-beam over
    // the zone is the usual cue that text can be entered.
    setCursor(Qt::IBeamCursor);

    m_icon = new QLabel(this);
    m_icon->setAlignment(Qt::AlignCenter);

    //@ DropZone
    //% "Drop a link here"
    m_title = new QLabel(qtTrId("dropzone.title"), this);
    m_title->setAlignment(Qt::AlignCenter);
    m_title->setFont(titleFont(font()));

    //@ DropZone
    //% "or press Ctrl+V to paste, Ctrl+L to type"
    m_hint = new QLabel(qtTrId("dropzone.hint"), this);
    m_hint->setObjectName(QStringLiteral("dropZoneHint"));
    m_hint->setAlignment(Qt::AlignCenter);
    // Secondary text takes the palette's placeholder color. Asking for the
    // role instead of copying the color into the palette means it is resolved
    // when the label paints, so it follows a switch to dark mode.
    m_hint->setForegroundRole(QPalette::PlaceholderText);

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

bool DropZone::event(QEvent *event)
{
    // QWidget handles DevicePixelRatioChange inline, before changeEvent() is
    // consulted, so re-rasterizing the icon is this override's job.
    if (event->type() == QEvent::DevicePixelRatioChange) {
        refreshIcon();
    }
    return QWidget::event(event);
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

void DropZone::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        Q_EMIT clicked();
    }
    QWidget::mousePressEvent(event);
}
