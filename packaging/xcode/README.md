# fsv.xcodeproj

An optional Xcode project for people who would rather hit Cmd-B than
type `meson`/`ninja` by hand. It is a thin wrapper, nothing more:

- The **Meson build stays the single source of truth**. `fsv`'s only
  target, `fsv`, is an **External Build System** target (Xcode calls
  this a "Legacy Target" in the raw project file). It has no compile
  phases, no source list, and no product Xcode itself links or signs —
  its entire "build" is one shell command that runs
  `meson setup … && ninja -C …` against a `builddir-xcode` directory
  next to the repo root.
- There is therefore **no Signing & Capabilities tab** for this
  target (External Build System targets have no product for Xcode to
  sign). "Sign to Run Locally" — the ad-hoc, no-team signing this
  project ships with — happens in `packaging/macos/make-bundle.sh`
  (see below), not in Xcode's UI.

## Requirements

- Xcode (full IDE, not just the Command Line Tools — `xcodebuild
  -version` should print something other than "requires Xcode").
- Homebrew `meson`, `ninja`, `sdl3`, `cglm` already installed (same
  dependencies the plain `meson setup builddir && ninja -C builddir`
  workflow needs — see the repo root `README.md`).

## Open and build

```sh
open packaging/xcode/fsv.xcodeproj
```

Select the `fsv` target/scheme and Build (Cmd-B), or from the command
line:

```sh
xcodebuild -project packaging/xcode/fsv.xcodeproj -target fsv -configuration Release build
```

This runs `meson setup builddir-xcode --buildtype=release` (only the
first time — `meson setup` errors on an existing build directory, so
the build script checks `test -d` first) followed by
`ninja -C builddir-xcode`, exactly mirroring the bare command line
workflow. The `Debug` configuration passes `--buildtype=debugoptimized`
instead. The resulting binary lands at
`builddir-xcode/src/sdl/fsv` — Xcode does not move or wrap it.

To pick up a build-type change on an existing checkout, delete
`builddir-xcode` first (`rm -rf builddir-xcode`) — same as you would
with any other Meson build directory.

## Building an `.app` bundle

Xcode's target above only produces the bare `fsv` binary, same as the
CLI Meson workflow. To get a double-clickable `fsv.app`:

```sh
meson setup builddir && ninja -C builddir   # or use builddir-xcode from above
packaging/macos/make-bundle.sh
open fsv.app
```

`make-bundle.sh` copies the built binary and
`packaging/macos/Info.plist` into a standard bundle layout and
ad-hoc code-signs it (`codesign --force --deep --sign -`) — the
scripted equivalent of "Sign to Run Locally". This is deliberately a
separate, explicit step rather than a build phase wired into the Xcode
target: it keeps the pbxproj minimal (no fake compile/copy phases) and
the bundling step useful to people who never open Xcode at all. See
`packaging/macos/make-bundle.sh --help`-style usage comments at the
top of the script for the `[builddir] [output.app]` arguments.

The bundle is **not** self-contained: `make-bundle.sh` copies the binary
in as-is, and that binary links Homebrew's `libglib-2.0` and `libSDL3`
by their `/opt/homebrew/opt/…` install paths (`otool -L fsv.app/Contents/
MacOS/fsv`). So `fsv.app` runs on a machine that has `brew install glib
sdl3`, and fails to launch with a dyld "Library not loaded" error on one
that doesn't. Relocating those dylibs into `Contents/Frameworks` with
`install_name_tool`/`dylibbundler` would make it distributable; that is
deliberately out of scope for now (see `docs/PORTING.md`).

No `fsv.icns` is checked in — the only icon asset in this repo is
`src/xmaps/fsv-icon.xpm`, a legacy GTK XPM that isn't a viable `.icns`
source without a hand-drawn multi-resolution PNG set. If someone adds
`packaging/macos/fsv.icns` later, `make-bundle.sh` already picks it up
automatically (see the script).

## If you have an Apple Developer team

Nothing above is affected by having a paid account — it's purely
additive:

1. In `Info.plist`, nothing needs to change (bundle identifier
   `net.sourceforge.fsv` is a placeholder; change it if you plan to
   distribute under your own identity).
2. Sign the bundle `make-bundle.sh` produces with your own identity
   instead of ad-hoc:
   ```sh
   codesign --force --deep --sign "Apple Development: you@example.com (TEAMID)" fsv.app
   ```
   (`security find-identity -v -p codesigning` lists what's available
   locally.)
3. For notarization/distribution outside your own Mac, you'd also need
   to enable the hardened runtime (`--options runtime`) and notarize
   via `xcrun notarytool` — out of scope for this project, which only
   targets local, unsigned use.

The Xcode project itself needs no changes for any of this, since it
never touches signing — only `make-bundle.sh`'s `codesign` invocation
does.
