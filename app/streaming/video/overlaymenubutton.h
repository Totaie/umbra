#pragma once

#include <QRasterWindow>
#include <QPainter>
#include <QMouseEvent>
#include <QSurfaceFormat>
#include <functional>

/**
 * OverlayMenuButton - A small floating button rendered by the OS compositor,
 * positioned at the top-right corner of the streaming window.
 *
 * When clicked, triggers a callback to open the overlay menu.
 * Semi-transparent when idle, fully opaque on hover.
 * Independent of D3D11/SDL rendering pipeline.
 */
class OverlayMenuButton : public QRasterWindow {
    Q_OBJECT
    Q_PROPERTY(qreal opacity READ opacity WRITE setOpacity)

public:
    using ClickCallback = std::function<void()>;

    explicit OverlayMenuButton(QWindow* parent = nullptr);
    ~OverlayMenuButton() override;

    void setClickCallback(ClickCallback cb) { m_ClickCallback = std::move(cb); }

    /**
     * Reposition the button relative to the given parent rect (SDL pixel coords).
     * Places the button at the top-right corner of the streaming window.
     */
    void repositionTo(int parentX, int parentY, int parentW, int parentH);

    /**
     * Show the button at the top-right corner of the given parent rect.
     */
    void showButton(int parentX, int parentY, int parentW, int parentH);

    /**
     * Hide the button.
     */
    void hideButton();

    /**
     * Re-assert topmost. A fullscreen window redrawing underneath will otherwise end
     * up above this one, and a button you cannot click is not a button.
     */
    void keepOnTop();

    /**
     * Called when a drag finishes, with the button's new position as a fraction of the
     * stream window. Wired to the preference so it survives the session.
     */
    using MovedCallback = std::function<void(qreal fracX, qreal fracY)>;
    void setMovedCallback(MovedCallback cb) { m_MovedCallback = std::move(cb); }

    /**
     * Where to sit, as a fraction of the parent rect. 1,0 is the top-right corner.
     */
    void setPositionFraction(qreal fracX, qreal fracY);

    bool isButtonVisible() const { return m_ButtonVisible; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    bool event(QEvent* event) override;

private:
    void drawCrescentMoon(QPainter& p, qreal cx, qreal cy, qreal radius);

    ClickCallback m_ClickCallback;
    MovedCallback m_MovedCallback;
    bool m_Hovered;
    bool m_ButtonVisible;

    // Drag state. A press only becomes a drag once it has moved far enough that it
    // can't be an unsteady click, otherwise the button would be impossible to press.
    bool m_Pressed;
    bool m_Dragging;
    QPoint m_PressPos;

    // Where it sits, as a fraction of the parent rect
    qreal m_FracX;
    qreal m_FracY;

    // The rect it was last positioned against, needed to turn a drag back into a fraction
    int m_ParentX, m_ParentY, m_ParentW, m_ParentH;

    static constexpr int kDragThreshold = 4;

    // Button size (logical pixels)
    static constexpr int kButtonSize = 36;
    static constexpr int kMargin = 10;  // margin from window edge
};
