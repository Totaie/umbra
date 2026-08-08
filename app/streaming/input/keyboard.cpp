#include "streaming/session.h"

#include <Limelight.h>
#include "SDL_compat.h"

#define VK_0 0x30
#define VK_A 0x41

// These are real Windows VK_* codes
#ifndef VK_F1
#define VK_F1 0x70
#define VK_F13 0x7C
#define VK_NUMPAD0 0x60
#endif

// Left-hand modifier VKEYs, matching what we send for real modifier key presses
#define VK_LSHIFT_LOCAL 0xA0
#define VK_LCONTROL_LOCAL 0xA2
#define VK_LMENU_LOCAL 0xA4

// Host shortcut VKEYs. See apply_shortcut() in the host's src/input.cpp.
#define VK_HOST_TOGGLE_CURSOR 0x4E  // 'N'
#define VK_HOST_HIDE_CURSOR 0x4F    // 'O', sets rather than toggles
#define VK_HOST_SHOW_CURSOR 0x50    // 'P'
#define VK_HOST_DISPLAY_FIRST 0x70  // F1 selects display 0, F2 display 1, ...
#define HOST_MAX_SWITCHABLE_DISPLAYS 13

void SdlInputHandler::sendHostShortcut(short keyCode)
{
    // The host only treats a key as a shortcut when Ctrl, Alt and Shift are all held,
    // and it tracks that from the modifier key events themselves rather than from the
    // modifier flags on the packet. So the modifiers have to be genuinely pressed and
    // released around the key instead of just setting the flags.
    const int allMods = MODIFIER_CTRL | MODIFIER_ALT | MODIFIER_SHIFT;

    LiSendKeyboardEvent(VK_LCONTROL_LOCAL, KEY_ACTION_DOWN, MODIFIER_CTRL);
    LiSendKeyboardEvent(VK_LMENU_LOCAL, KEY_ACTION_DOWN, MODIFIER_CTRL | MODIFIER_ALT);
    LiSendKeyboardEvent(VK_LSHIFT_LOCAL, KEY_ACTION_DOWN, allMods);

    LiSendKeyboardEvent(keyCode, KEY_ACTION_DOWN, allMods);
    LiSendKeyboardEvent(keyCode, KEY_ACTION_UP, allMods);

    LiSendKeyboardEvent(VK_LSHIFT_LOCAL, KEY_ACTION_UP, MODIFIER_CTRL | MODIFIER_ALT);
    LiSendKeyboardEvent(VK_LMENU_LOCAL, KEY_ACTION_UP, MODIFIER_CTRL);
    LiSendKeyboardEvent(VK_LCONTROL_LOCAL, KEY_ACTION_UP, 0);
}

void SdlInputHandler::hideHostCursor()
{
    // Sets rather than toggles, so asking twice is harmless and a reconnect can't
    // leave the host drawing its pointer under the one we draw ourselves. The old
    // toggle is what made the host cursor reappear at random.
    //
    // A host too old to know this shortcut passes the key through to the desktop
    // instead, which is untidy but harmless; Umbra ships both halves together.
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Asking host to stop drawing its cursor");
    sendHostShortcut(VK_HOST_HIDE_CURSOR);
}

void SdlInputHandler::showHostCursor()
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Asking host to draw its cursor again");
    sendHostShortcut(VK_HOST_SHOW_CURSOR);
}

void SdlInputHandler::switchHostDisplay(int displayIndex)
{
    if (displayIndex < 0 || displayIndex >= HOST_MAX_SWITCHABLE_DISPLAYS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Ignoring out of range host display index: %d", displayIndex);
        return;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Switching host to display %d", displayIndex);
    sendHostShortcut(VK_HOST_DISPLAY_FIRST + displayIndex);
    m_CurrentHostDisplay = displayIndex;

    // Bring the pointer with us. The host tears capture down and brings it back up on
    // the other display, and in absolute mode nothing tells it where the pointer is
    // until the pointer moves - so without this you arrive on the new display and the
    // mouse is still wherever it was on the old one until you jiggle it.
    if (m_Window != nullptr && m_AbsoluteMouseMode) {
        int ww, wh;
        SDL_GetWindowSize(m_Window, &ww, &wh);
        SDL_WarpMouseInWindow(m_Window, ww / 2, wh / 2);
    }
}

void SdlInputHandler::setHostDisplays(int count, int current)
{
    m_HostDisplayCount = count;
    m_CurrentHostDisplay = current;
}

void SdlInputHandler::cycleHostDisplay()
{
    // A host that never answered leaves the count at zero. Guess two rather than do
    // nothing: the host clamps an index past its last display instead of failing, so
    // guessing high on a single-display host just switches it to the display it is
    // already on, and guessing low on a three-display host still reaches two of them.
    int count = m_HostDisplayCount > 0 ? m_HostDisplayCount : 2;
    switchHostDisplay((m_CurrentHostDisplay + 1) % count);
}

void SdlInputHandler::setLocalCursorVisible(bool visible)
{
    m_MouseCursorCapturedVisibilityState = visible ? SDL_ENABLE : SDL_DISABLE;

    // In relative mode the cursor is captured and must stay hidden. Setting the state
    // is still worthwhile so it applies if the user toggles to absolute mode.
    if (!SDL_GetRelativeMouseMode()) {
        SDL_ShowCursor(m_MouseCursorCapturedVisibilityState);
    }
}

void SdlInputHandler::performSpecialKeyCombo(KeyCombo combo)
{
    switch (combo) {
    case KeyComboQuit:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected quit key combo");

        // Push a quit event to the main loop
        SDL_Event event;
        event.type = SDL_QUIT;
        event.quit.timestamp = SDL_GetTicks();
        SDL_PushEvent(&event);
        break;

    case KeyComboUngrabInput:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected mouse capture toggle combo");

        // Stop handling future input
        setCaptureActive(!isCaptureActive());

        // Force raise all keys to ensure they aren't stuck,
        // since we won't get their key up events.
        raiseAllKeys();
        break;

    case KeyComboToggleFullScreen:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected full-screen toggle combo");
        Session::s_ActiveSession->toggleFullscreen();

        // Force raise all keys just be safe across this full-screen/windowed
        // transition just in case key events get lost.
        raiseAllKeys();
        break;

    case KeyComboToggleStatsOverlay:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected stats toggle combo");

        // Toggle the stats overlay
        Session::get()->getOverlayManager().setOverlayState(Overlay::OverlayDebug,
                                                            !Session::get()->getOverlayManager().isOverlayEnabled(Overlay::OverlayDebug));
        break;

    case KeyComboToggleMouseMode:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected mouse mode toggle combo");

        // Uncapture input
        setCaptureActive(false);

        // Toggle mouse mode
        m_AbsoluteMouseMode = !m_AbsoluteMouseMode;
        synchronizeLocalCursorMode();

        // Recapture input
        setCaptureActive(true);
        break;

    case KeyComboToggleCursorHide:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected show mouse combo");

        if (!SDL_GetRelativeMouseMode()) {
            // 切换本地鼠标光标可见性状态
            m_MouseCursorCapturedVisibilityState = !m_MouseCursorCapturedVisibilityState;
            synchronizeLocalCursorMode();
            applyCapturedCursorState();
        }
        else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Cursor can only be shown in remote desktop mouse mode");
        }
        break;

    case KeyComboToggleMinimize:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected minimize combo");
        SDL_MinimizeWindow(m_Window);
        break;

    case KeyComboPasteText:
    {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected type clipboard text combo");

        // Force raise all keys to ensure that none of them interfere
        // with the text we're going to type.
        raiseAllKeys();

        char* text;
        if (SDL_HasClipboardText() && (text = SDL_GetClipboardText()) != nullptr) {
            // Sending both CR and LF will lead to two newlines in the destination for
            // each newline in the source, so we fix up any CRLFs into just a single LF.
            for (char* c = text; *c != 0; c++) {
                if (*c == '\r' && *(c + 1) == '\n') {
                    // We're using strlen() rather than strlen() - 1 since we need to add 1
                    // to copy the null terminator which is not included in strlen()'s count.
                    memmove(c, c + 1, strlen(c));
                }
            }

            // Send this text to the PC
            LiSendUtf8TextEvent(text, (unsigned int)strlen(text));

            // SDL_GetClipboardText() allocates, so we must free
            SDL_free((void*)text);
        }
        else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "No text in clipboard to paste!");
        }
        break;
    }

    case KeyComboTogglePointerRegionLock:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected pointer region lock toggle combo");
        m_PointerRegionLockActive = !m_PointerRegionLockActive;

        // Remember that the user changed this manually, so we don't mess with it anymore
        // during windowed <-> full-screen transitions.
        m_PointerRegionLockToggledByUser = true;

        // Apply the new region lock
        updatePointerRegionLock();
        break;

    case KeyComboQuitAndExit:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected quitAndExit key combo");

        // Indicate that we want to exit afterwards
        Session::get()->setShouldExit(true);

        // Push a quit event to the main loop
        SDL_Event quitExitEvent;
        quitExitEvent.type = SDL_QUIT;
        quitExitEvent.quit.timestamp = SDL_GetTicks();
        SDL_PushEvent(&quitExitEvent);
        break;

    case KeyComboToggleKeyboardGrab:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected keyboard grab toggle combo");

        // Toggle the system key capture mode
        if (isSystemKeyCaptureActive()) {
            m_CaptureSystemKeysMode = StreamingPreferences::CSK_OFF;
        }
        else {
            m_CaptureSystemKeysMode = StreamingPreferences::CSK_ALWAYS;
        }

        updateKeyboardGrabState();
        break;

    default:
        Q_UNREACHABLE();
    }
}

void SdlInputHandler::handleKeyEvent(SDL_KeyboardEvent* event)
{
    short keyCode;
    char modifiers;
    bool shouldNotConvertToScanCodeOnServer = false;

    if (event->repeat) {
        // Ignore repeat key down events
        SDL_assert(event->state == SDL_PRESSED);
        return;
    }

    // Ctrl+Shift+D moves to the host's next display. Deliberately not the
    // Ctrl+Alt+Shift the combos below use: this is something you reach for in the
    // middle of working, and three modifiers plus a letter is not a one-handed
    // gesture. The cost is that the host never sees Ctrl+Shift+D itself.
    if ((event->state == SDL_PRESSED) &&
            (event->keysym.mod & KMOD_CTRL) &&
            (event->keysym.mod & KMOD_SHIFT) &&
            !(event->keysym.mod & KMOD_ALT) &&
            event->keysym.sym == SDLK_d) {
        cycleHostDisplay();
        return;
    }

    // Check for our special key combos
    if ((event->state == SDL_PRESSED) &&
            (event->keysym.mod & KMOD_CTRL) &&
            (event->keysym.mod & KMOD_ALT) &&
            (event->keysym.mod & KMOD_SHIFT)) {
        // First we test the SDLK combos for matches,
        // that way we ensure that latin keyboard users
        // can match to the key they see on their keyboards.
        // If nothing matches that, we'll then go on to
        // checking scancodes so non-latin keyboard users
        // can have working hotkeys (though possibly in
        // odd positions). We must do all SDLK tests before
        // any scancode tests to avoid issues in cases
        // where the SDLK for one shortcut collides with
        // the scancode of another.

        for (int i = 0; i < KeyComboMax; i++) {
            if (m_SpecialKeyCombos[i].enabled && event->keysym.sym == m_SpecialKeyCombos[i].keyCode) {
                performSpecialKeyCombo(m_SpecialKeyCombos[i].keyCombo);
                return;
            }
        }

        for (int i = 0; i < KeyComboMax; i++) {
            if (m_SpecialKeyCombos[i].enabled && event->keysym.scancode == m_SpecialKeyCombos[i].scanCode) {
                performSpecialKeyCombo(m_SpecialKeyCombos[i].keyCombo);
                return;
            }
        }
    }

    // Set modifier flags
    modifiers = 0;
    if (event->keysym.mod & KMOD_CTRL) {
        modifiers |= MODIFIER_CTRL;
    }
    if (event->keysym.mod & KMOD_ALT) {
        modifiers |= m_SwapWinAltKeys ? MODIFIER_META : MODIFIER_ALT;
    }
    if (event->keysym.mod & KMOD_SHIFT) {
        modifiers |= MODIFIER_SHIFT;
    }
    if (event->keysym.mod & KMOD_GUI) {
        if (isSystemKeyCaptureActive()) {
            modifiers |= m_SwapWinAltKeys ? MODIFIER_ALT : MODIFIER_META;
        }
    }

    // Set keycode. We explicitly use scancode here because GFE will try to correct
    // for AZERTY layouts on the host but it depends on receiving VK_ values matching
    // a QWERTY layout to work.
    if (event->keysym.scancode >= SDL_SCANCODE_1 && event->keysym.scancode <= SDL_SCANCODE_9) {
        // SDL defines SDL_SCANCODE_0 > SDL_SCANCODE_9, so we need to handle that manually
        keyCode = (event->keysym.scancode - SDL_SCANCODE_1) + VK_0 + 1;
    }
    else if (event->keysym.scancode >= SDL_SCANCODE_A && event->keysym.scancode <= SDL_SCANCODE_Z) {
        keyCode = (event->keysym.scancode - SDL_SCANCODE_A) + VK_A;
    }
    else if (event->keysym.scancode >= SDL_SCANCODE_F1 && event->keysym.scancode <= SDL_SCANCODE_F12) {
        keyCode = (event->keysym.scancode - SDL_SCANCODE_F1) + VK_F1;
    }
    else if (event->keysym.scancode >= SDL_SCANCODE_F13 && event->keysym.scancode <= SDL_SCANCODE_F24) {
        keyCode = (event->keysym.scancode - SDL_SCANCODE_F13) + VK_F13;
    }
    else if (event->keysym.scancode >= SDL_SCANCODE_KP_1 && event->keysym.scancode <= SDL_SCANCODE_KP_9) {
        // SDL defines SDL_SCANCODE_KP_0 > SDL_SCANCODE_KP_9, so we need to handle that manually
        keyCode = (event->keysym.scancode - SDL_SCANCODE_KP_1) + VK_NUMPAD0 + 1;
    }
    else {
        switch (event->keysym.scancode) {
            case SDL_SCANCODE_BACKSPACE:
                keyCode = 0x08;
                break;
            case SDL_SCANCODE_TAB:
                keyCode = 0x09;
                break;
            case SDL_SCANCODE_CLEAR:
                keyCode = 0x0C;
                break;
            case SDL_SCANCODE_KP_ENTER: // FIXME: Is this correct?
            case SDL_SCANCODE_RETURN:
                keyCode = 0x0D;
                break;
            case SDL_SCANCODE_PAUSE:
                keyCode = 0x13;
                break;
            case SDL_SCANCODE_CAPSLOCK:
                keyCode = 0x14;
                break;
            case SDL_SCANCODE_ESCAPE:
                keyCode = 0x1B;
                break;
            case SDL_SCANCODE_SPACE:
                keyCode = 0x20;
                break;
            case SDL_SCANCODE_PAGEUP:
                keyCode = 0x21;
                break;
            case SDL_SCANCODE_PAGEDOWN:
                keyCode = 0x22;
                break;
            case SDL_SCANCODE_END:
                keyCode = 0x23;
                break;
            case SDL_SCANCODE_HOME:
                keyCode = 0x24;
                break;
            case SDL_SCANCODE_LEFT:
                keyCode = 0x25;
                break;
            case SDL_SCANCODE_UP:
                keyCode = 0x26;
                break;
            case SDL_SCANCODE_RIGHT:
                keyCode = 0x27;
                break;
            case SDL_SCANCODE_DOWN:
                keyCode = 0x28;
                break;
            case SDL_SCANCODE_SELECT:
                keyCode = 0x29;
                break;
            case SDL_SCANCODE_EXECUTE:
                keyCode = 0x2B;
                break;
            case SDL_SCANCODE_PRINTSCREEN:
                keyCode = 0x2C;
                break;
            case SDL_SCANCODE_INSERT:
                keyCode = 0x2D;
                break;
            case SDL_SCANCODE_DELETE:
                keyCode = 0x2E;
                break;
            case SDL_SCANCODE_HELP:
                keyCode = 0x2F;
                break;
            case SDL_SCANCODE_KP_0:
                // See comment above about why we only handle SDL_SCANCODE_KP_0 here
                keyCode = VK_NUMPAD0;
                break;
            case SDL_SCANCODE_0:
                // See comment above about why we only handle SDL_SCANCODE_0 here
                keyCode = VK_0;
                break;
            case SDL_SCANCODE_KP_MULTIPLY:
                keyCode = 0x6A;
                break;
            case SDL_SCANCODE_KP_PLUS:
                keyCode = 0x6B;
                break;
            case SDL_SCANCODE_KP_COMMA:
                keyCode = 0x6C;
                break;
            case SDL_SCANCODE_KP_MINUS:
                keyCode = 0x6D;
                break;
            case SDL_SCANCODE_KP_PERIOD:
                keyCode = 0x6E;
                break;
            case SDL_SCANCODE_KP_DIVIDE:
                keyCode = 0x6F;
                break;
            case SDL_SCANCODE_NUMLOCKCLEAR:
                keyCode = 0x90;
                break;
            case SDL_SCANCODE_SCROLLLOCK:
                keyCode = 0x91;
                break;
            case SDL_SCANCODE_LSHIFT:
                keyCode = 0xA0;
                break;
            case SDL_SCANCODE_RSHIFT:
                keyCode = 0xA1;
                break;
            case SDL_SCANCODE_LCTRL:
                keyCode = 0xA2;
                break;
            case SDL_SCANCODE_RCTRL:
                keyCode = 0xA3;
                break;
            case SDL_SCANCODE_LALT:
                keyCode = m_SwapWinAltKeys ? 0x5B : 0xA4;
                break;
            case SDL_SCANCODE_RALT:
                keyCode = m_SwapWinAltKeys ? 0x5C : 0xA5;
                break;
            case SDL_SCANCODE_LGUI:
                if (!isSystemKeyCaptureActive()) {
                    return;
                }
                keyCode = m_SwapWinAltKeys ? 0xA4 : 0x5B;
                break;
            case SDL_SCANCODE_RGUI:
                if (!isSystemKeyCaptureActive()) {
                    return;
                }
                keyCode = m_SwapWinAltKeys ? 0xA5 : 0x5C;
                break;
            case SDL_SCANCODE_APPLICATION:
                keyCode = 0x5D;
                break;
            case SDL_SCANCODE_AC_BACK:
                keyCode = 0xA6;
                break;
            case SDL_SCANCODE_AC_FORWARD:
                keyCode = 0xA7;
                break;
            case SDL_SCANCODE_AC_REFRESH:
                keyCode = 0xA8;
                break;
            case SDL_SCANCODE_AC_STOP:
                keyCode = 0xA9;
                break;
            case SDL_SCANCODE_AC_SEARCH:
                keyCode = 0xAA;
                break;
            case SDL_SCANCODE_AC_BOOKMARKS:
                keyCode = 0xAB;
                break;
            case SDL_SCANCODE_AC_HOME:
                keyCode = 0xAC;
                break;
            case SDL_SCANCODE_SEMICOLON:
                keyCode = 0xBA;
                break;
            case SDL_SCANCODE_EQUALS:
                keyCode = 0xBB;
                break;
            case SDL_SCANCODE_COMMA:
                keyCode = 0xBC;
                break;
            case SDL_SCANCODE_MINUS:
                keyCode = 0xBD;
                break;
            case SDL_SCANCODE_PERIOD:
                keyCode = 0xBE;
                break;
            case SDL_SCANCODE_SLASH:
                keyCode = 0xBF;
                break;
            case SDL_SCANCODE_GRAVE:
                keyCode = 0xC0;
                break;
            case SDL_SCANCODE_LEFTBRACKET:
                keyCode = 0xDB;
                break;
            case SDL_SCANCODE_INTERNATIONAL3:
                shouldNotConvertToScanCodeOnServer = true;
                Q_FALLTHROUGH();
            case SDL_SCANCODE_BACKSLASH:
                keyCode = 0xDC;
                break;
            case SDL_SCANCODE_RIGHTBRACKET:
                keyCode = 0xDD;
                break;
            case SDL_SCANCODE_APOSTROPHE:
                keyCode = 0xDE;
                break;
            case SDL_SCANCODE_INTERNATIONAL1:
                shouldNotConvertToScanCodeOnServer = true;
                Q_FALLTHROUGH();
            case SDL_SCANCODE_NONUSBACKSLASH:
                keyCode = 0xE2;
                break;
            case SDL_SCANCODE_LANG1:
                keyCode = 0x1C;
                break;
            case SDL_SCANCODE_LANG2:
                keyCode = 0x1D;
                break;
            default:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Unhandled button event: %d",
                             event->keysym.scancode);
                return;
        }
    }

    // Track the key state so we always know which keys are down
    if (event->state == SDL_PRESSED) {
        m_KeysDown.insert(keyCode);
    }
    else {
        m_KeysDown.remove(keyCode);
    }

    const char keyAction = event->state == SDL_PRESSED ? KEY_ACTION_DOWN : KEY_ACTION_UP;
    const char flags = shouldNotConvertToScanCodeOnServer ? SS_KBE_FLAG_NON_NORMALIZED : 0;

    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                 "Keyboard %s: scancode=%d sym=%d vk=0x%04x modifiers=0x%02x flags=0x%02x keysDown=%d",
                 event->state == SDL_PRESSED ? "down" : "up",
                 (int)event->keysym.scancode,
                 (int)event->keysym.sym,
                 (int)(0x8000 | keyCode),
                 (int)(unsigned char)modifiers,
                 (int)(unsigned char)flags,
                 (int)m_KeysDown.count());

    int rc = LiSendKeyboardEvent2(0x8000 | keyCode,
                                  keyAction,
                                  modifiers,
                                  flags);
    if (rc != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "LiSendKeyboardEvent2 failed: rc=%d action=%s scancode=%d vk=0x%04x keysDown=%d",
                    rc,
                    keyAction == KEY_ACTION_DOWN ? "down" : "up",
                    (int)event->keysym.scancode,
                    (int)(0x8000 | keyCode),
                    (int)m_KeysDown.count());
    }
}
