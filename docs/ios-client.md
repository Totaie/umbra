# Umbra on iOS

Notes for building an Umbra client that installs as an `.IPA` on iOS 26/27.

## This repository cannot become an IPA

`umbra` is a fork of `moonlight-qt`, the Qt/QML desktop client. Moonlight's iOS client is a
completely separate codebase: [moonlight-stream/moonlight-ios](https://github.com/moonlight-stream/moonlight-ios),
written in Objective-C and C against UIKit, sharing only `moonlight-common-c` with the desktop client.

So an iOS Umbra means **forking `moonlight-ios` and porting the Umbra features across**, not building
this repository for a new target. Qt does support iOS, but porting this QML UI would mean rebuilding
the touch input, video decode (VideoToolbox) and on-screen controller layers that `moonlight-ios`
already has, for a worse result.

`moonlight-ios` is GPL-3.0, so a fork inherits the same licence Umbra already uses.

## Hard constraints, worth knowing before starting

**A Mac is required.** Building and signing an IPA needs Xcode, which is macOS-only. There is no
Windows path — not a cross-compiler, not a CI trick that avoids it. GitHub Actions offers hosted
macOS runners, which is a workable route if you don't own a Mac, but signing still needs an Apple
developer identity.

**The App Store is not an option.** GPLv3 is incompatible with the App Store's terms — the licence
grants rights the terms forbid, which is why VLC was pulled years ago. Distribution has to be one of:

| Route | Cost | Reinstall interval | Notes |
|---|---|---|---|
| Apple Developer Program | $99/yr | 1 year | Sign it yourself, install via Xcode or Apple Configurator |
| Free Apple ID | free | **7 days** | Fine for testing, painful for daily use |
| AltStore / SideStore | free | 7 days, auto-renewed on Wi-Fi | SideStore renews without a computer nearby |
| TestFlight | $99/yr | 90 days | Apple reviews the build; GPLv3 makes this risky |

For personal use on your own phone, SideStore with a free Apple ID is the usual answer, and the paid
developer account if the weekly re-signing gets annoying.

**Upstream is somewhat stale.** `moonlight-ios` was last pushed October 2025. It may need work to
build against a current Xcode and iOS SDK before any Umbra features are added. Budget for that first.

## Which Umbra features port, and how

The good news: the two host-side features are driven entirely through the input channel, so they are
protocol-level and work from any client.

| Feature | Portability | Notes |
|---|---|---|
| Host display switching | **Direct** | Send `Ctrl+Alt+Shift+F1`…`F13` as key events. Identical to the desktop implementation; see `sendHostShortcut()` in `app/streaming/input/keyboard.cpp`. |
| Client-drawn cursor | **Direct** | Same `Ctrl+Alt+Shift+N` chord to stop the host compositing its cursor. iOS already draws its own pointer in trackpad mode, so this is a clear win on iPad with a mouse. |
| Straight to the desktop | **Easy** | Same idea as `AppModel::getDirectLaunchAppIndex()`: pick the host's "Desktop" app instead of showing the app grid. |
| Immersive mode | **Not applicable** | iOS has no Windows key to capture and won't let an app grab system gestures. |
| Multi-display | **Not applicable** | One screen. |
| Custom background | **Easy** | UI work, no shared logic. |

The two "direct" features are the valuable ones on iPad with a Magic Keyboard, and they are the
cheapest to port because the host already accepts the chords.

## Suggested order

1. Fork `moonlight-ios` to `Totaie/umbra-ios`.
2. Get it building unmodified against current Xcode. Fix whatever bit-rotted. Do not add features yet.
3. Get it signed and running on the target device via SideStore, so the install loop is proven.
4. Port straight-to-desktop, then the two chord-driven features.
5. Rebrand last — it is the least interesting part and touches the most files.

Do not start at step 5.
