#include "overlaymenubutton.h"

#include <QScreen>
#include <QPainterPath>

OverlayMenuButton::OverlayMenuButton(QWindow* parent)
    : QRasterWindow(parent),
      m_Hovered(false),
      m_ButtonVisible(false),
      m_Pressed(false),
      m_Dragging(false),
      m_FracX(1.0),
      m_FracY(0.0),
      m_ParentX(0), m_ParentY(0), m_ParentW(0), m_ParentH(0)
{
    setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
             | Qt::WindowDoesNotAcceptFocus);

    QSurfaceFormat fmt;
    fmt.setAlphaBufferSize(8);
    setFormat(fmt);

    setOpacity(0.35);
}

OverlayMenuButton::~OverlayMenuButton()
{
}

void OverlayMenuButton::setPositionFraction(qreal fracX, qreal fracY)
{
    m_FracX = qBound(0.0, fracX, 1.0);
    m_FracY = qBound(0.0, fracY, 1.0);

    if (m_ParentW > 0 && m_ParentH > 0) {
        repositionTo(m_ParentX, m_ParentY, m_ParentW, m_ParentH);
    }
}

void OverlayMenuButton::repositionTo(int parentX, int parentY, int parentW, int parentH)
{
    m_ParentX = parentX;
    m_ParentY = parentY;
    m_ParentW = parentW;
    m_ParentH = parentH;

#ifdef Q_OS_MACOS
    int qpX = parentX;
    int qpY = parentY;
    int qpW = parentW;
    int qpH = parentH;
#else
    qreal dpr = screen() ? screen()->devicePixelRatio() : 1.0;
    int qpX = qRound(parentX / dpr);
    int qpY = qRound(parentY / dpr);
    int qpW = qRound(parentW / dpr);
    int qpH = qRound(parentH / dpr);
#endif

    // The fraction spans the area the button can actually occupy, so 1.0 puts its
    // right edge on the window's right edge rather than pushing it off-screen.
    int travelX = qMax(0, qpW - kButtonSize - 2 * kMargin);
    int travelY = qMax(0, qpH - kButtonSize - 2 * kMargin);

    int x = qpX + kMargin + qRound(m_FracX * travelX);
    int y = qpY + kMargin + qRound(m_FracY * travelY);

    setGeometry(x, y, kButtonSize, kButtonSize);
}

void OverlayMenuButton::keepOnTop()
{
    if (!m_ButtonVisible) {
        return;
    }

    // Asking once at show() isn't enough against a fullscreen window that keeps
    // presenting; this is called from the same place the overlay windows are synced.
    raise();
}

void OverlayMenuButton::showButton(int parentX, int parentY, int parentW, int parentH)
{
    repositionTo(parentX, parentY, parentW, parentH);
    m_ButtonVisible = true;
    show();
    raise();
    requestUpdate();
}

void OverlayMenuButton::hideButton()
{
    m_ButtonVisible = false;
    hide();
}

void OverlayMenuButton::drawCrescentMoon(QPainter& p, qreal cx, qreal cy, qreal radius)
{
    // Crescent moon: full circle minus an offset circle
    QPainterPath moonPath;
    moonPath.addEllipse(QPointF(cx, cy), radius, radius);

    QPainterPath cutout;
    cutout.addEllipse(QPointF(cx + radius * 0.5, cy - radius * 0.25), radius * 0.78, radius * 0.82);

    QPainterPath crescent = moonPath.subtracted(cutout);

    // Soft golden glow
    QColor moonColor = m_Hovered ? QColor(255, 235, 140) : QColor(230, 215, 150);
    p.fillPath(crescent, moonColor);
}

void OverlayMenuButton::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    int w = width();
    int h = height();

    // Clear to transparent
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(0, 0, w, h, Qt::transparent);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);

    qreal cx = w / 2.0;
    qreal cy = h / 2.0;
    qreal bgRadius = qMin(w, h) / 2.0 - 1.0;

    // Circular dark background
    QPainterPath bgCircle;
    bgCircle.addEllipse(QPointF(cx, cy), bgRadius, bgRadius);

    QColor bgColor = m_Hovered ? QColor(35, 40, 75, 230) : QColor(20, 24, 50, 200);
    p.fillPath(bgCircle, bgColor);

    // Subtle border
    QColor borderColor = m_Hovered ? QColor(120, 150, 230, 150) : QColor(70, 85, 150, 80);
    p.setPen(QPen(borderColor, 1.0));
    p.drawPath(bgCircle);

    // Draw crescent moon centered in the background
    qreal moonR = bgRadius * 0.55;
    drawCrescentMoon(p, cx, cy, moonR);
}

void OverlayMenuButton::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        return;
    }

    // The click fires on release, not here, because until the pointer either moves or
    // comes back up there is no way to know whether this is a press or a drag.
    m_Pressed = true;
    m_Dragging = false;
    m_PressPos = event->globalPosition().toPoint() - position();
}

void OverlayMenuButton::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_Hovered) {
        m_Hovered = true;
        setOpacity(0.95);
        requestUpdate();
    }

    if (!m_Pressed) {
        return;
    }

    QPoint target = event->globalPosition().toPoint() - m_PressPos;

    if (!m_Dragging) {
        // Far enough that it can't be a shaky click
        if ((target - position()).manhattanLength() < kDragThreshold) {
            return;
        }
        m_Dragging = true;
    }

    setPosition(target);
}

void OverlayMenuButton::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        return;
    }

    bool wasDragging = m_Dragging;
    m_Pressed = false;
    m_Dragging = false;

    if (!wasDragging) {
        if (m_ClickCallback) {
            m_ClickCallback();
        }
        return;
    }

    // Convert back to a fraction of the window so it stays where it was put even if
    // the stream comes back at another size, or on another monitor.
#ifdef Q_OS_MACOS
    qreal dpr = 1.0;
#else
    qreal dpr = screen() ? screen()->devicePixelRatio() : 1.0;
#endif
    int qpX = qRound(m_ParentX / dpr);
    int qpY = qRound(m_ParentY / dpr);
    int qpW = qRound(m_ParentW / dpr);
    int qpH = qRound(m_ParentH / dpr);

    int travelX = qMax(1, qpW - kButtonSize - 2 * kMargin);
    int travelY = qMax(1, qpH - kButtonSize - 2 * kMargin);

    m_FracX = qBound(0.0, (qreal)(x() - qpX - kMargin) / travelX, 1.0);
    m_FracY = qBound(0.0, (qreal)(y() - qpY - kMargin) / travelY, 1.0);

    // Snap back inside if it was dragged past an edge
    repositionTo(m_ParentX, m_ParentY, m_ParentW, m_ParentH);

    if (m_MovedCallback) {
        m_MovedCallback(m_FracX, m_FracY);
    }
}

bool OverlayMenuButton::event(QEvent* ev)
{
    if (ev->type() == QEvent::Leave) {
        m_Hovered = false;
        setOpacity(0.35);
        requestUpdate();
    }
    return QRasterWindow::event(ev);
}
