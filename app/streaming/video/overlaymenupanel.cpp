#include "overlaymenupanel.h"
#include "uifont.h"

#include <QScreen>
#include <QGuiApplication>
#include <QCoreApplication>
#include <QPainterPath>
#include <QCursor>
#include <QFontDatabase>
#include <memory>

OverlayMenuPanel::OverlayMenuPanel(QWindow* parent)
    : QRasterWindow(parent),
      m_CurrentLevel(0),
      m_HoveredIndex(-1),
      m_Visible(false),
      m_HasGamepads(false),
      m_FileMappingState(FileMappingState::Unknown),
      m_CurrentHostDisplay(0),
      m_HasMultipleScreens(false),
      m_BitrateKbps(0),
      m_MicrophoneOn(false),
      m_GamepadMouseOn(false),
      m_FileMappingDetail(tr("Checking")),
      m_ParentX(0), m_ParentY(0), m_ParentW(0), m_ParentH(0),
      m_ContentOffset(0),
      m_Closing(false),
      m_TargetX(0),
      m_CursorX(0), m_CursorY(0)
{
    setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
             | Qt::WindowDoesNotAcceptFocus);

    QSurfaceFormat fmt;
    fmt.setAlphaBufferSize(8);
    setFormat(fmt);

    // Logical (unscaled) values - Qt 6 handles DPI automatically.
    //
    // Square, like everything else in Umbra. This arrived from upstream wearing a
    // Windows 11 context menu - rounded, grey, a soft gradient shadow - in an app whose
    // whole visual language is hard corners, a solid offset shadow and one teal accent.
    m_ItemHeight   = 38;
    m_Padding      = 6;
    m_MenuWidth    = 310;
    m_BorderRadius = 0;
    m_ShadowMargin = 10;
    m_TitleHeight  = 36;
    m_IconAreaWidth = 0;   // no icon column; the sections carry the grouping now
    m_HeaderHeight = 30;
    m_BandHeight   = 38;

    // The same face as the rest of the interface.
    //
    // This used to be ModeSeven, the bitmap font the performance stats overlay draws
    // with. That reads as a teletype from a different decade sitting on top of the
    // app, which was tolerable while the app itself was proportional and clearly a
    // separate thing - now that everything is monospaced, matching it is what makes
    // the menu belong to Umbra rather than to the video underneath it.
    m_LabelFont.setFamilies(UiFont::familyChain(QStringLiteral("DM Mono")));
    m_LabelFont.setStyleHint(QFont::Monospace);
    m_LabelFont.setPointSize(9);
    m_LabelFont.setWeight(QFont::Normal);

    m_DetailFont = QFont(m_LabelFont);
    m_DetailFont.setPointSize(8);
    m_DetailFont.setWeight(QFont::Normal);

    // Section headers and the band across the top. Wide-tracked uppercase, the same
    // micro-label treatment the settings rows and card captions use.
    m_TitleFont = QFont(m_LabelFont);
    m_TitleFont.setPointSize(8);
    m_TitleFont.setWeight(QFont::DemiBold);
    m_TitleFont.setLetterSpacing(QFont::AbsoluteSpacing, 1.6);

    // Icon font: platform-specific
#ifdef Q_OS_WIN
    // Segoe MDL2 Assets — available on Windows 10/11
    m_IconFont = QFont(QStringLiteral("Segoe MDL2 Assets"), 10);
#else
    // Material Icons (bundled, Apache 2.0) — cross-platform fallback
    {
        int iconFontId = QFontDatabase::addApplicationFont(QStringLiteral(":/data/MaterialIcons-Regular.ttf"));
        QString materialFamily;
        if (iconFontId >= 0) {
            QStringList families = QFontDatabase::applicationFontFamilies(iconFontId);
            if (!families.isEmpty())
                materialFamily = families.first();
        }
        if (!materialFamily.isEmpty())
            m_IconFont = QFont(materialFamily, 12);
        else
            m_IconFont = QFont(QStringLiteral("Material Icons"), 12);
    }
#endif
    m_IconFont.setWeight(QFont::Normal);

    // --- Animations ---
    m_OpacityAnim = new QPropertyAnimation(this, "opacity", this);
    m_SlideAnim   = new QPropertyAnimation(this, "x", this);

    m_ContentSlideAnim = new QVariantAnimation(this);
    m_ContentSlideAnim->setDuration(150);
    m_ContentSlideAnim->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_ContentSlideAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant& val) {
        m_ContentOffset = val.toReal();
        forceRepaint();
    });
    connect(m_ContentSlideAnim, &QVariantAnimation::finished, this, [this]() {
        m_ContentOffset = 0;
        forceRepaint();
    });

    m_LeaveTimer.setSingleShot(true);
    connect(&m_LeaveTimer, &QTimer::timeout, this, [this]() {
        if (m_Visible && !geometry().contains(QCursor::pos())) {
            closeMenu();
        }
    });

    buildMenuLevels();
}

OverlayMenuPanel::~OverlayMenuPanel()
{
}

// ---------------------------------------------------------------------------
// Synchronous repaint — requestUpdate() is async on Windows and may delay
// the visual update by up to 1 second (until the next SDL event arrives).
// This method directly delivers an UpdateRequest event so the paintEvent()
// runs immediately within the current call frame.
// ---------------------------------------------------------------------------

void OverlayMenuPanel::forceRepaint()
{
    if (isExposed()) {
        // Mark entire window as dirty (sets the dirty region for QPaintDeviceWindow)
        update(QRect(0, 0, width(), height()));
        // Synchronously deliver UpdateRequest to trigger paintEvent + backing store flush
        QEvent ev(QEvent::UpdateRequest);
        QCoreApplication::sendEvent(this, &ev);
    }
}

// ---------------------------------------------------------------------------
// Menu structure
// ---------------------------------------------------------------------------

void OverlayMenuPanel::buildMenuLevels()
{
    m_MenuLevels.clear();

    auto header = [](const QString& text) {
        return MenuItem {text, QString(), MenuItemType::Header,
                         MenuAction::MenuActionMax, 0, false, false, false};
    };

    // === Level 0: everything worth reaching without navigating ===
    MenuLevel top;
    top.title = QStringLiteral("Umbra");

    // --- Display ---
    // Only worth the section when there is a choice to make, or somewhere to put a
    // second window.
    if (m_HostDisplays.size() > 1 || m_HasMultipleScreens) {
        top.items.push_back(header(tr("Display")));

        const int selectable = qMin(m_HostDisplays.size(),
                                    (int)MenuAction::SwitchDisplay7 - (int)MenuAction::SwitchDisplay0 + 1);
        for (int i = 0; i < selectable; i++) {
            top.items.push_back({m_HostDisplays.at(i),
                                 i == m_CurrentHostDisplay ? QStringLiteral("\u2713") : QString(),
                                 MenuItemType::Action,
                                 (MenuAction)((int)MenuAction::SwitchDisplay0 + i),
                                 0, true, false, false});
        }

        if (m_HostDisplays.size() > 1) {
            top.items.push_back({tr("Next Display"), QStringLiteral("Ctrl+Shift+D"),
                                 MenuItemType::Action, MenuAction::SwitchDisplayNext,
                                 0, true, false, false});
        }

        if (m_HasMultipleScreens) {
            top.items.push_back({tr("Stream to All Screens"), QString(),
                                 MenuItemType::Action, MenuAction::StreamAllScreens,
                                 0, true, false, false});
        }
    }

    // --- Session ---
    top.items.push_back(header(tr("Session")));
    top.items.push_back({tr("Fullscreen"), QStringLiteral("Ctrl+Alt+Shift+X"),
                         MenuItemType::Action, MenuAction::ToggleFullScreen,
                         0, true, false, false});
    top.items.push_back({tr("Microphone"), QString(), MenuItemType::Toggle,
                         MenuAction::ToggleMicrophone, 0, true, m_MicrophoneOn, false});
    if (m_HasGamepads) {
        top.items.push_back({tr("Gamepad Mouse"), QString(), MenuItemType::Toggle,
                             MenuAction::ToggleGamepadMouse, 0, true, m_GamepadMouseOn, false});
    }
    top.items.push_back({tr("Performance Stats"), QStringLiteral("Ctrl+Alt+Shift+S"),
                         MenuItemType::Action, MenuAction::ToggleStatsOverlay,
                         0, true, false, false});
    top.items.push_back({tr("Host Files"), m_FileMappingDetail, MenuItemType::Action,
                         MenuAction::ShowHostFiles, 0, true,
                         m_FileMappingState == FileMappingState::Available ||
                         m_FileMappingState == FileMappingState::Open, false});

    // --- Everything else ---
    top.items.push_back(header(tr("More")));
    top.items.push_back({tr("Bitrate"), bitrateLabel(), MenuItemType::SubMenu,
                         MenuAction::MenuActionMax, 2, true, false, false});
    top.items.push_back({tr("Other Shortcuts"), QString(), MenuItemType::SubMenu,
                         MenuAction::MenuActionMax, 1, true, false, false});
    top.items.push_back({tr("Disconnect"), QStringLiteral("Ctrl+Alt+Shift+Q"),
                         MenuItemType::Action, MenuAction::Quit, 0, true, false, false});
    m_MenuLevels.push_back(top);

    // === Level 1: the shortcuts you reach for rarely ===
    MenuLevel shortcuts;
    shortcuts.title = tr("Other Shortcuts");
    shortcuts.items.push_back({tr("Mouse Mode"), QStringLiteral("Ctrl+Alt+Shift+M"),
                               MenuItemType::Action, MenuAction::ToggleMouseMode, 0, true, false, false});
    shortcuts.items.push_back({tr("Show/Hide Cursor"), QStringLiteral("Ctrl+Alt+Shift+C"),
                               MenuItemType::Action, MenuAction::ToggleCursorHide, 0, true, false, false});
    shortcuts.items.push_back({tr("Pointer Region Lock"), QStringLiteral("Ctrl+Alt+Shift+L"),
                               MenuItemType::Action, MenuAction::TogglePointerRegionLock, 0, true, false, false});
    shortcuts.items.push_back({tr("Ungrab Mouse"), QStringLiteral("Ctrl+Alt+Shift+Z"),
                               MenuItemType::Action, MenuAction::UngrabInput, 0, true, false, false});
    shortcuts.items.push_back({tr("Paste Clipboard"), QStringLiteral("Ctrl+Alt+Shift+V"),
                               MenuItemType::Action, MenuAction::PasteText, 0, true, false, false});
    shortcuts.items.push_back({tr("Minimize"), QStringLiteral("Ctrl+Alt+Shift+D"),
                               MenuItemType::Action, MenuAction::ToggleMinimize, 0, true, false, false});
    shortcuts.items.push_back({tr("Quit Umbra"), QStringLiteral("Ctrl+Alt+Shift+E"),
                               MenuItemType::Action, MenuAction::QuitAndExit, 0, true, false, false});
    m_MenuLevels.push_back(shortcuts);

    // === Level 2: Bitrate presets ===
    MenuLevel bitrate;
    bitrate.title = tr("Bitrate");
    static const int kPresets[] = {1000, 2000, 5000, 10000, 20000, 30000, 50000, 100000};
    for (int i = 0; i < 8; i++) {
        bitrate.items.push_back({kPresets[i] >= 1000 ? tr("%1 Mbps").arg(kPresets[i] / 1000)
                                                     : tr("%1 Kbps").arg(kPresets[i]),
                                 m_BitrateKbps == kPresets[i] ? QStringLiteral("\u2713") : QString(),
                                 MenuItemType::Action,
                                 (MenuAction)((int)MenuAction::SetBitrate1000 + i),
                                 0, true, false, false});
    }
    m_MenuLevels.push_back(bitrate);
}

QString OverlayMenuPanel::bitrateLabel() const
{
    if (m_BitrateKbps <= 0) {
        return QString();
    }
    return m_BitrateKbps >= 1000 ? tr("%1 Mbps").arg(m_BitrateKbps / 1000)
                                 : tr("%1 Kbps").arg(m_BitrateKbps);
}

int OverlayMenuPanel::itemHeight(const MenuItem& item) const
{
    return item.type == MenuItemType::Header ? m_HeaderHeight : m_ItemHeight;
}

int OverlayMenuPanel::levelHeight(const MenuLevel& level) const
{
    int total = 0;
    for (const auto& item : level.items) {
        total += itemHeight(item);
    }
    return total;
}

void OverlayMenuPanel::setSessionInfo(const QString& hostName, const QString& streamMode)
{
    m_HostName = hostName;
    m_StreamMode = streamMode;
}

void OverlayMenuPanel::setHasMultipleScreens(bool has)
{
    if (m_HasMultipleScreens != has) {
        m_HasMultipleScreens = has;
        buildMenuLevels();
    }
}

void OverlayMenuPanel::setHostDisplays(const QStringList& names, int current)
{
    if (m_HostDisplays == names && m_CurrentHostDisplay == current) {
        return;
    }

    m_HostDisplays = names;
    m_CurrentHostDisplay = current;
    buildMenuLevels();
}

// ---------------------------------------------------------------------------
// Dynamic state updates
// ---------------------------------------------------------------------------

void OverlayMenuPanel::updateMicrophoneState(bool enabled)
{
    if (m_MicrophoneOn == enabled) {
        return;
    }

    m_MicrophoneOn = enabled;
    buildMenuLevels();
    forceRepaint();
}

void OverlayMenuPanel::updateGamepadMouseState(bool enabled)
{
    if (m_GamepadMouseOn == enabled) {
        return;
    }

    m_GamepadMouseOn = enabled;
    buildMenuLevels();
    forceRepaint();
}

void OverlayMenuPanel::updateBitrateState(int bitrateKbps)
{
    if (m_BitrateKbps == bitrateKbps) {
        return;
    }

    // Rebuild rather than reach in and edit the rows: the label appears in two places
    // (the Bitrate row's detail and the tick in the presets), and keeping those in step
    // by hand is how they drift.
    m_BitrateKbps = bitrateKbps;
    buildMenuLevels();
}

void OverlayMenuPanel::updateFileMappingState(FileMappingState state, const QString& detail)
{
    m_FileMappingState = state;
    m_FileMappingDetail = detail;

    if (m_MenuLevels.empty()) return;
    for (auto& item : m_MenuLevels[0].items) {
        if (item.action == MenuAction::ShowHostFiles) {
            item.detail = detail;
            item.toggleState = state == FileMappingState::Available ||
                               state == FileMappingState::Open;
            forceRepaint();
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Show / hide / navigate
// ---------------------------------------------------------------------------

void OverlayMenuPanel::showAtCursor(int parentX, int parentY, int parentW, int parentH,
                                     int cursorX, int cursorY)
{
    m_ParentX = parentX;
    m_ParentY = parentY;
    m_ParentW = parentW;
    m_ParentH = parentH;
    m_CursorX = cursorX;
    m_CursorY = cursorY;
    showInternal();
}

void OverlayMenuPanel::showInternal()
{
    m_LeaveTimer.stop();
    m_CurrentLevel = 0;
    m_HoveredIndex = -1;
    m_ContentOffset = 0;

    // If closing animation is in progress, cancel it
    if (m_Closing) {
        m_OpacityAnim->stop();
        m_SlideAnim->stop();
        m_Closing = false;
    }

    m_Visible = true;
    m_ShowTimer.start();

    // Calculate target geometry
    repositionWindow();
    m_TargetX = x();

    // Slides in from the right, toward the button it hangs off
    int slideDistance = 40;
    int slideDir = 1;
    setPosition(m_TargetX + slideDistance * slideDir, y());
    setOpacity(0.0);

    show();
    raise();

    // Animate slide
    m_SlideAnim->setDuration(220);
    m_SlideAnim->setStartValue(m_TargetX + slideDistance * slideDir);
    m_SlideAnim->setEndValue(m_TargetX);
    m_SlideAnim->setEasingCurve(QEasingCurve::OutCubic);

    // Animate opacity: 0 → 1
    m_OpacityAnim->setDuration(220);
    m_OpacityAnim->setStartValue(0.0);
    m_OpacityAnim->setEndValue(1.0);
    m_OpacityAnim->setEasingCurve(QEasingCurve::OutCubic);

    m_SlideAnim->start();
    m_OpacityAnim->start();

    forceRepaint();
}

void OverlayMenuPanel::repositionWindow()
{
#ifdef Q_OS_MACOS
    // On macOS, SDL and Qt both use points (logical coordinates)
    int qpX = m_ParentX;
    int qpY = m_ParentY;
    int qpW = m_ParentW;
    int qpH = m_ParentH;
#else
    // On other platforms, convert SDL pixel coordinates to Qt DIP
    qreal dpr = screen() ? screen()->devicePixelRatio() : 1.0;
    int qpX = qRound(m_ParentX / dpr);
    int qpY = qRound(m_ParentY / dpr);
    int qpW = qRound(m_ParentW / dpr);
    int qpH = qRound(m_ParentH / dpr);
#endif

    int bandH      = (m_CurrentLevel > 0) ? m_TitleHeight : m_BandHeight;
    int menuHeight = bandH + levelHeight(m_MenuLevels[m_CurrentLevel]) + m_Padding * 2;

    // Content top-left, hung under the point the menu was opened from
#ifdef Q_OS_MACOS
    int qcX = m_CursorX;
    int qcY = m_CursorY;
#else
    int qcX = qRound(m_CursorX / dpr);
    int qcY = qRound(m_CursorY / dpr);
#endif
    int cx = qcX;
    int cy = qcY;

    // Clamp within parent bounds
    if (cx + m_MenuWidth > qpX + qpW) cx = qpX + qpW - m_MenuWidth;
    if (cx < qpX) cx = qpX;

    // Clamp vertical position within parent
    if (cy < qpY) cy = qpY;
    if (cy + menuHeight > qpY + qpH) cy = qpY + qpH - menuHeight;

    // Window includes shadow margin around content
    setGeometry(cx - m_ShadowMargin, cy - m_ShadowMargin,
                m_MenuWidth + 2 * m_ShadowMargin, menuHeight + 2 * m_ShadowMargin);
}

void OverlayMenuPanel::navigateToLevel(int level)
{
    if (level < 0 || level >= (int)m_MenuLevels.size()) return;

    m_LeaveTimer.stop();
    bool goingForward = level > m_CurrentLevel;
    m_ContentSlideAnim->stop();
    m_ContentOffset = 0;

    m_CurrentLevel = level;
    m_HoveredIndex = -1;
    repositionWindow();

    // Reset grace period so Leave event won't close the menu immediately
    // (the mouse may be outside the resized window after navigation)
    m_ShowTimer.start();

    // Warp cursor into the new menu if it's now outside
    QPoint globalPos = QCursor::pos();
    if (!geometry().contains(globalPos)) {
        QCursor::setPos(geometry().center());
    }

    if (goingForward) {
        // Forward: content slides in from right
        m_ContentSlideAnim->setStartValue(30.0);
        m_ContentSlideAnim->setEndValue(0.0);
        m_ContentSlideAnim->start();
    } else {
        // Back: instant switch, no animation (avoids jarring resize + slide combo)
        forceRepaint();
    }
}

void OverlayMenuPanel::closeMenu()
{
    m_LeaveTimer.stop();
    if (!m_Visible) return;
    if (m_Closing) return;  // already animating close

    m_Visible = false;
    m_Closing = true;
    m_HoveredIndex = -1;

    // Stop any show/level animations
    m_SlideAnim->stop();
    m_OpacityAnim->stop();
    m_ContentSlideAnim->stop();
    m_ContentOffset = 0;

    // Animate slide out: current x → +30px right
    int slideDistance = 30;
    m_SlideAnim->setDuration(160);
    m_SlideAnim->setStartValue(x());
    m_SlideAnim->setEndValue(x() + slideDistance);
    m_SlideAnim->setEasingCurve(QEasingCurve::InCubic);

    // Animate opacity: current → 0
    m_OpacityAnim->setDuration(160);
    m_OpacityAnim->setStartValue(opacity());
    m_OpacityAnim->setEndValue(0.0);
    m_OpacityAnim->setEasingCurve(QEasingCurve::InCubic);

    // When fade-out completes, finalize (use disconnect to emulate single-shot for Qt 5 compat)
    auto conn = std::make_shared<QMetaObject::Connection>();
    *conn = connect(m_OpacityAnim, &QPropertyAnimation::finished, this, [this, conn]() {
        disconnect(*conn);
        m_Closing = false;
        m_CurrentLevel = 0;
        hide();
        setOpacity(1.0);   // reset for next show
        if (m_CloseCallback) {
            m_CloseCallback();
        }
    });

    m_SlideAnim->start();
    m_OpacityAnim->start();
}

int OverlayMenuPanel::itemAtPos(const QPoint& pos) const
{
    // Adjust for shadow margin
    int lx = pos.x() - m_ShadowMargin;
    int ly = pos.y() - m_ShadowMargin;
    if (lx < 0 || lx >= m_MenuWidth || ly < 0) return -1;

    // The band across the top names the session on level 0 and goes back on sub-levels
    int bandH = (m_CurrentLevel > 0) ? m_TitleHeight : m_BandHeight;
    if (ly < bandH) {
        return m_CurrentLevel > 0 ? -2 : -1;
    }

    int localY = ly - bandH - m_Padding;
    if (localY < 0) return -1;

    // Walk, because section headers are shorter than the rest
    const auto& items = m_MenuLevels[m_CurrentLevel].items;
    int y = 0;
    for (int i = 0; i < (int)items.size(); i++) {
        int h = itemHeight(items[i]);
        if (localY < y + h) {
            // Headers are labels, not targets
            return items[i].type == MenuItemType::Header ? -1 : i;
        }
        y += h;
    }

    return -1;
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void OverlayMenuPanel::paintEvent(QPaintEvent*)
{
    // Theme.qml, so this matches every other surface in the app rather than the
    // Windows 11 context menu it arrived as.
    const QColor kSurface   (0x17, 0x1A, 0x20);
    const QColor kSurface2  (0x1F, 0x23, 0x2B);
    const QColor kLine      (0x2B, 0x30, 0x38);
    const QColor kLineStrong(0x3C, 0x43, 0x4E);
    const QColor kText      (0xEE, 0xF0, 0xEC);
    const QColor kTextDim   (0x8B, 0x8F, 0x86);
    const QColor kTextFaint (0x5C, 0x61, 0x69);
    const QColor kAccent    (0x39, 0xC5, 0xBB);
    const QColor kDanger    (0xFF, 0x87, 0x6F);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);

    int w = width();
    int h = height();
    int sm = m_ShadowMargin;
    int cw = w - 2 * sm;
    int ch = h - 2 * sm;

    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(0, 0, w, h, Qt::transparent);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);

    // Zero blur, pure offset - Theme.shadowOffset / Theme.shadowColor
    p.fillRect(QRectF(sm + 6, sm + 6, cw, ch), QColor(0, 0, 0, 140));

    p.save();
    p.translate(sm, sm);

    p.fillRect(QRectF(0, 0, cw, ch), kSurface);
    p.setPen(QPen(kLineStrong, 1.0));
    p.drawRect(QRectF(0.5, 0.5, cw - 1, ch - 1));
    p.setClipRect(QRectF(0, 0, cw, ch));

    const auto& level = m_MenuLevels[m_CurrentLevel];
    const int textPad = 14;

    // --- Band across the top --------------------------------------------------
    // Level 0 names the session; deeper levels are the way back.
    int bandH = (m_CurrentLevel > 0) ? m_TitleHeight : m_BandHeight;

    if (m_CurrentLevel > 0) {
        bool hovered = (m_HoveredIndex == -2);
        if (hovered) {
            p.fillRect(QRectF(1, 1, cw - 2, bandH - 2), kSurface2);
        }
        p.setFont(m_TitleFont);
        p.setPen(hovered ? kText : kTextDim);
        p.drawText(QRect(textPad, 0, cw - 2 * textPad, bandH),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QString::fromUtf8("\xe2\x97\x82  ") + level.title.toUpper());
    }
    else {
        p.setFont(m_TitleFont);
        p.setPen(kTextDim);
        p.drawText(QRect(textPad, 0, cw - 2 * textPad, bandH),
                   Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("UMBRA"));

        // Which machine, and at what, on the right of the same line
        QString right = m_HostName;
        if (!m_StreamMode.isEmpty()) {
            right += right.isEmpty() ? m_StreamMode
                                     : QString::fromUtf8("  \xc2\xb7  ") + m_StreamMode;
        }
        if (!right.isEmpty()) {
            p.setFont(m_DetailFont);
            p.setPen(kTextFaint);
            p.drawText(QRect(textPad, 0, cw - 2 * textPad, bandH),
                       Qt::AlignRight | Qt::AlignVCenter, right);
        }
    }

    p.setPen(QPen(kLine, 1.0));
    p.drawLine(0, bandH, cw, bandH);

    // Slide offset used while moving between levels
    if (m_ContentSlideAnim->state() != QAbstractAnimation::Running) {
        m_ContentOffset = 0;
    }
    p.save();
    if (m_ContentOffset != 0) {
        p.translate(m_ContentOffset, 0);
    }

    const auto& items = level.items;
    int itemY = bandH + m_Padding;

    for (int i = 0; i < (int)items.size(); i++) {
        const auto& item = items[i];
        const int rowH = itemHeight(item);

        // --- Section header ---
        if (item.type == MenuItemType::Header) {
            p.setFont(m_TitleFont);
            p.setPen(kTextFaint);
            p.drawText(QRect(textPad, itemY, cw - 2 * textPad, rowH),
                       Qt::AlignLeft | Qt::AlignBottom, item.label.toUpper());
            itemY += rowH;
            continue;
        }

        bool hovered = (i == m_HoveredIndex);
        bool isDanger = (item.action == MenuAction::Quit ||
                         item.action == MenuAction::QuitAndExit);

        if (hovered) {
            p.fillRect(QRectF(1, itemY, cw - 2, rowH), kSurface2);

            // A bar down the left edge, the way the cards mark themselves
            p.fillRect(QRectF(1, itemY, 3, rowH), isDanger ? kDanger : kAccent);
        }

        QColor labelColor = isDanger ? kDanger : (item.enabled ? kText : kTextFaint);
        if (!item.enabled) {
            labelColor = kTextFaint;
        }

        // Room on the right for whatever the row carries
        int rightReserve = 0;
        if (item.type == MenuItemType::Toggle) {
            rightReserve = 40;
        }
        else if (item.type == MenuItemType::SubMenu) {
            rightReserve = 14;
        }

        p.setFont(m_LabelFont);
        p.setPen(labelColor);
        QRect labelRect(textPad, itemY, cw - textPad * 2 - rightReserve, rowH);

        // The detail column shares the row, so measure the label and let the detail
        // have what's left. Long shortcut strings otherwise overprint the label.
        int labelW = p.fontMetrics().horizontalAdvance(item.label);
        p.drawText(labelRect, Qt::AlignLeft | Qt::AlignVCenter, item.label);

        // --- Right-hand column ---
        if (item.type == MenuItemType::Toggle) {
            // Square, like every other control here
            const int trackW = 26, trackH = 14;
            int trackX = cw - textPad - trackW;
            int trackY = itemY + (rowH - trackH) / 2;

            p.setPen(QPen(item.toggleState ? kAccent : kLineStrong, 1.0));
            p.setBrush(Qt::NoBrush);
            p.drawRect(QRectF(trackX + 0.5, trackY + 0.5, trackW - 1, trackH - 1));

            p.setPen(Qt::NoPen);
            p.setBrush(item.toggleState ? kAccent : kTextFaint);
            int knob = 10;
            int knobX = item.toggleState ? trackX + trackW - knob - 2 : trackX + 2;
            p.drawRect(QRect(knobX, trackY + (trackH - knob) / 2, knob, knob));
            p.setBrush(Qt::NoBrush);
        }
        else if (item.type == MenuItemType::SubMenu) {
            if (!item.detail.isEmpty()) {
                p.setFont(m_DetailFont);
                p.setPen(kTextDim);
                p.drawText(QRect(textPad + labelW + 12, itemY,
                                 cw - textPad * 2 - labelW - 12 - 14, rowH),
                           Qt::AlignRight | Qt::AlignVCenter, item.detail);
            }
            p.setFont(m_LabelFont);
            p.setPen(kTextFaint);
            p.drawText(QRect(cw - textPad - 10, itemY, 10, rowH),
                       Qt::AlignCenter, QString::fromUtf8("\xe2\x80\xba"));
        }
        else if (!item.detail.isEmpty()) {
            // A tick marks the live choice; anything else is a shortcut or a status
            bool isCheck = (item.detail == QString::fromUtf8("\xe2\x9c\x93"));
            p.setFont(isCheck ? m_LabelFont : m_DetailFont);
            p.setPen(isCheck ? kAccent : kTextFaint);
            p.drawText(QRect(textPad + labelW + 12, itemY,
                             cw - textPad * 2 - labelW - 12, rowH),
                       Qt::AlignRight | Qt::AlignVCenter, item.detail);
        }

        itemY += rowH;
    }

    p.restore();  // content offset
    p.restore();  // shadow margin
}

// ---------------------------------------------------------------------------
// Mouse input
// ---------------------------------------------------------------------------

void OverlayMenuPanel::mouseMoveEvent(QMouseEvent* event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    int newIdx = itemAtPos(event->position().toPoint());
#else
    int newIdx = itemAtPos(event->pos());
#endif
    if (newIdx != m_HoveredIndex) {
        m_HoveredIndex = newIdx;
        setCursor((m_HoveredIndex >= 0 || m_HoveredIndex == -2) ? Qt::PointingHandCursor : Qt::ArrowCursor);
        forceRepaint();
    }
}

void OverlayMenuPanel::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) return;

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    int idx = itemAtPos(event->position().toPoint());
#else
    int idx = itemAtPos(event->pos());
#endif

    // Title bar click → navigate back
    if (idx == -2) {
        navigateToLevel(0);
        return;
    }

    if (idx < 0) return;

    const auto& items = m_MenuLevels[m_CurrentLevel].items;
    if (idx >= (int)items.size() || !items[idx].enabled) return;

    const auto& item = items[idx];

    switch (item.type) {
    case MenuItemType::Header:
        // Not selectable; gamepad navigation skips these anyway
        break;
    case MenuItemType::Back:
        navigateToLevel(0);
        break;

    case MenuItemType::SubMenu:
        navigateToLevel(item.targetLevel);
        break;

    case MenuItemType::Action:
    {
        MenuAction action = item.action;
        closeMenu();
        if (m_ActionCallback) {
            m_ActionCallback(action);
        }
        break;
    }

    case MenuItemType::Toggle:
    {
        // Toggle visual state and dispatch
        auto& mutableItem = m_MenuLevels[m_CurrentLevel].items[idx];
        mutableItem.toggleState = !mutableItem.toggleState;
        forceRepaint();
        if (m_ActionCallback) {
            m_ActionCallback(item.action);
        }
        break;
    }
    }
}

// ---------------------------------------------------------------------------
// Gamepad navigation
// ---------------------------------------------------------------------------

void OverlayMenuPanel::gamepadMoveUp()
{
    if (!m_Visible) return;
    const auto& items = m_MenuLevels[m_CurrentLevel].items;
    if (items.empty()) return;

    if (m_HoveredIndex <= 0) {
        // Wrap to last item, or move to title bar if on sub-level
        if (m_CurrentLevel > 0 && m_HoveredIndex == 0) {
            m_HoveredIndex = -2; // title bar (back button)
        } else {
            m_HoveredIndex = (int)items.size() - 1;
        }
    } else {
        m_HoveredIndex--;
    }
    // Skip disabled items
    if (m_HoveredIndex >= 0 && !items[m_HoveredIndex].enabled) {
        gamepadMoveUp();
        return;
    }
    forceRepaint();
}

void OverlayMenuPanel::gamepadMoveDown()
{
    if (!m_Visible) return;
    const auto& items = m_MenuLevels[m_CurrentLevel].items;
    if (items.empty()) return;

    if (m_HoveredIndex == -2) {
        // From title bar, move to first item
        m_HoveredIndex = 0;
    } else if (m_HoveredIndex < 0 || m_HoveredIndex >= (int)items.size() - 1) {
        // Wrap to title bar on sub-level, or to first item on top level
        if (m_CurrentLevel > 0) {
            m_HoveredIndex = -2;
        } else {
            m_HoveredIndex = 0;
        }
    } else {
        m_HoveredIndex++;
    }
    // Skip disabled items
    if (m_HoveredIndex >= 0 && !items[m_HoveredIndex].enabled) {
        gamepadMoveDown();
        return;
    }
    forceRepaint();
}

void OverlayMenuPanel::gamepadSelect()
{
    if (!m_Visible) return;

    // Title bar → back
    if (m_HoveredIndex == -2) {
        navigateToLevel(0);
        return;
    }

    if (m_HoveredIndex < 0) return;

    const auto& items = m_MenuLevels[m_CurrentLevel].items;
    if (m_HoveredIndex >= (int)items.size() || !items[m_HoveredIndex].enabled) return;

    const auto& item = items[m_HoveredIndex];

    switch (item.type) {
    case MenuItemType::Back:
        navigateToLevel(0);
        break;
    case MenuItemType::SubMenu:
        navigateToLevel(item.targetLevel);
        break;
    case MenuItemType::Action:
    {
        MenuAction action = item.action;
        closeMenu();
        if (m_ActionCallback) {
            m_ActionCallback(action);
        }
        break;
    }
    case MenuItemType::Toggle:
    {
        auto& mutableItem = m_MenuLevels[m_CurrentLevel].items[m_HoveredIndex];
        mutableItem.toggleState = !mutableItem.toggleState;
        forceRepaint();
        if (m_ActionCallback) {
            m_ActionCallback(item.action);
        }
        break;
    }
    }
}

void OverlayMenuPanel::gamepadBack()
{
    if (!m_Visible) return;

    if (m_CurrentLevel > 0) {
        navigateToLevel(0);
    } else {
        closeMenu();
    }
}

bool OverlayMenuPanel::event(QEvent* ev)
{
    if (ev->type() == QEvent::Leave) {
        if (m_Visible) {
            // During the grace period, defer the outside check instead of
            // dropping the Leave event. Otherwise, leaving the panel quickly
            // after it opens would keep it visible until the cursor entered
            // and left the panel again.
            constexpr qint64 LeaveGracePeriodMs = 300;
            const qint64 elapsed = m_ShowTimer.elapsed();
            if (elapsed < LeaveGracePeriodMs) {
                m_LeaveTimer.start(static_cast<int>(LeaveGracePeriodMs - elapsed + 1));
                return true;
            }
            // Verify cursor is actually outside (cursor warp may lag)
            QPoint globalPos = QCursor::pos();
            if (geometry().contains(globalPos)) {
                return true;
            }
            closeMenu();
        }
        return true;
    }
    return QRasterWindow::event(ev);
}
