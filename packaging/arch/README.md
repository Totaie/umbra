# Umbra on Arch Linux

Two ways to get it. Build from a checkout to try something that has not been
released; use the pacman repository for everything that has, and never think
about updating again.

## Build and install from a checkout

```sh
sudo pacman -S --needed git base-devel
git clone --branch umbra-v2 https://github.com/Totaie/umbra.git
cd umbra/packaging/arch
makepkg -si
```

Swap `umbra-v2` for whichever branch you were asked to test.

`makepkg -si` does all of it: installs the build dependencies through pacman
(this is where it asks for your password), fetches the submodules, compiles for
a few minutes, and installs the result as the package `umbra`. Run it as your
own user, never as root.

Do not run `git submodule update --init --recursive` first. It fails on this
repository, and the PKGBUILD already calls `scripts/fetch-submodules.sh`, which
explains why and works around it.

To pick up later changes on the same branch:

```sh
git pull
makepkg -sif
```

The `f` is needed because the version number may not have changed, and makepkg
otherwise declines to rebuild a package it thinks it already has.

## Check that it works

Start it from a terminal the first time, so its log is in front of you:

```sh
umbra
```

1. Your host should appear by itself if it is on the same network; otherwise add
   it by address. Pair with the PIN it shows.
2. Start a stream. `Ctrl+Alt+Shift+S` shows the statistics overlay, and the
   decoder line in it says whether video is being decoded in hardware.
3. With the stream full-screen, `Ctrl+Alt+Shift+Tab` minimizes it and
   `Ctrl+Alt+Shift+Q` disconnects.
4. Copy some text on this machine and paste it on the host with `Ctrl+V`, then
   copy something on the host and paste it here: the two share one clipboard.
   Umbra Host and Apollo share text only. A Foundation Sunshine host shares
   images too, though on Wayland an image only crosses from the host to here,
   and only where XWayland is running.

The shortcuts reach Umbra because a full-screen stream captures system keys.
GNOME asks once whether Umbra may do that; say yes. In a window the desktop
keeps its own shortcuts unless "capture system keys" is set to always.

### Hardware decoding

Umbra uses VA-API, and falls back to decoding in software when there is no
driver for it. That works, at the cost of CPU and some latency. The driver is
`mesa` for AMD, `intel-media-driver` for Intel, and `libva-nvidia-driver` for
NVIDIA. `vainfo`, from `libva-utils`, lists what the driver offers; H.264 and
HEVC decode entries (`VAEntrypointVLD`) are the ones that matter.

## Updates through pacman

Once a version has been released, add Umbra's repository, once:

```sh
printf '\n[umbra]\nSigLevel = Optional TrustAll\nServer = https://github.com/Totaie/umbra/releases/download/arch\n' | sudo tee -a /etc/pacman.conf
sudo pacman -Syu umbra
```

From then on Umbra updates with the rest of the system on `sudo pacman -Syu`.
A build installed from a checkout is the same package, so the repository simply
takes over from it at the next version.

The packages are not signed, which is what `SigLevel = Optional TrustAll` allows
for this one repository. If Umbra stops starting right after a system update
with an error about a missing `libav*.so` or `libplacebo.so`, Arch has moved to
a new version of that library and the package needs rebuilding against it:
`makepkg -sif` from a checkout fixes it on the spot.

## Removing it

```sh
sudo pacman -Rns umbra
```

Then delete the `[umbra]` block from `/etc/pacman.conf` if you added it.
Settings and paired hosts are in `~/.config/Umbra Project/`.

## Handing the install to an agent

Paste this into a coding agent running on the Arch machine, with the branch
filled in:

```text
Install and smoke-test a development build of Umbra, a Moonlight-based remote
desktop and game streaming client, on this Arch Linux machine.

Source: https://github.com/Totaie/umbra, branch BRANCH. Read
packaging/arch/README.md on that branch first and follow it; where it disagrees
with this prompt, it wins.

Do this:
1. Make sure git and base-devel are installed.
2. Clone the branch to ~/src/umbra. If that directory already exists, update it
   with git pull instead of cloning again.
3. In packaging/arch, run: makepkg -si   (add f if the package was built before)
4. Verify the install: `pacman -Qi umbra` reports the version in app/version.txt;
   `ldd /usr/bin/umbra | grep "not found"` prints nothing; then run `umbra` from
   a terminal for about 15 seconds, keep its output, and confirm it loads its
   interface, chooses a video decoder, and does not crash.
5. Check hardware decoding with `vainfo` and tell me which driver is in use, or
   which package would provide one for this GPU.

Rules:
- Never run makepkg as root, and do not run
  `git submodule update --init --recursive`: it fails on this repository by
  design, and the PKGBUILD fetches the submodules correctly by itself.
- sudo needs my password. If you cannot prompt for it, stop and give me the
  exact command to run myself.
- Install nothing beyond what makepkg asks for without asking me first, and do
  not add the [umbra] repository to pacman.conf: this is a test of an
  unreleased build.
- If the build fails, show me the first real error rather than the last lines,
  and stop. Do not patch the source to get past it.

Finish with a short report: the installed version, the desktop and session type
(X11 or Wayland), the GPU and the decoder Umbra chose, any warnings or errors in
the startup output, and a reminder of what I still need to test by hand - pair
with the host, start a stream, and in full-screen press Ctrl+Alt+Shift+Tab to
minimize and Ctrl+Alt+Shift+Q to disconnect.
```
