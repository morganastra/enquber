#pragma once

#include "qrcode.h"

#include <QImage>
#include <QWidget>

/// Shows a QR symbol as large as it fits.
///
/// The symbol is rendered at a whole number of device pixels per module
/// whenever the size changes, so the modules never land on half pixels and the
/// code stays sharp and scannable at any window size or scaling factor.
class QrView : public QWidget
{
    Q_OBJECT

public:
    explicit QrView(QWidget *parent = nullptr);

    void setCode(const qr::Code &code);
    void clear();
    bool hasCode() const { return m_code.isValid(); }

    /// Highlights the code; used while a drop is hovering over the window.
    void setHighlighted(bool highlighted);

    QSize minimumSizeHint() const override;
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void dropCache();

    qr::Code m_code;
    QImage m_cache;
    int m_cacheModulePixels = 0;
    bool m_highlighted = false;
};
