# FSV

> **metal-port branch** — this branch **is** the macOS/Metal port of fsv: a
> native SDL3 GPU (Metal) renderer with a Dear ImGui interface, replacing
> the GTK3 + OpenGL frontend on macOS. The legacy GTK/OpenGL frontend is
> preserved and keeps building on Linux. See [`docs/PORTING.md`](docs/PORTING.md)
> for the full porting record. For the original GTK/OpenGL-only project,
> use [jabl/fsv](https://github.com/jabl/fsv).

![fsv flying its own source tree in fsn mode, ending on the squarified MapV](docs/media/demo.gif)

*(GIF above is size-optimized — 15fps, downscaled. See
[`docs/media/demo.mp4`](docs/media/demo.mp4) for the smooth, full-resolution
version.)*

This repo is a fork of [fsv](http://fsv.sourceforge.net/), updated to current environments.
The original author is [Daniel Richard G.](https://github.com/iskunk), a former student of Computer Science at the MIT.

## About fsv

> fsv (pronounced eff-ess-vee) is a file system visualizer in cyberspace. It lays out files and directories in three dimensions, geometrically representing the file system hierarchy to allow visual overview and analysis. fsv can visualize a modest home directory, a workstation's hard drive, or any arbitrarily large collection of files, limited only by the host computer's memory and graphics hardware.

Its ancestor, SGI's `fsn` (pronounced "fusion") originated on IRIX and was prominently featured in Jurassic Park: ["It's a Unix system!"](https://www.youtube.com/watch?v=3HjOjvu6oKA).

[Screenshots](http://fsv.sourceforge.net/screenshots/) of the original clone are still available.

Useful info and screenshots of the original SGI IRIX implementation are available on [siliconbunny](http://www.siliconbunny.com/fsn-the-irix-3d-file-system-tool-from-jurassic-park/).

## Install

### macOS (primary)

```sh
brew install glib cglm meson ninja pkgconf sdl3
meson setup builddir
ninja -C builddir
./builddir/src/sdl/fsv [dir]
```

`frontend=sdl` (the SDL3/Metal renderer, this port) is the default on
every platform. `[dir]` defaults to the current directory if omitted.

- **`.app` bundle** (optional, for double-clicking instead of running from
  a terminal): `packaging/macos/make-bundle.sh` copies the binary built
  above into a standard bundle layout, bundles its runtime dylib closure
  (SDL3, glib, and their own dependencies) into `Contents/Frameworks` so
  the result doesn't depend on your Homebrew install either, and
  ad-hoc code-signs it — `packaging/macos/make-bundle.sh && open
  fsv.app`. See [`packaging/macos/make-bundle.sh`](packaging/macos/make-bundle.sh)
  for details (also used to build the release tarball below); pass
  `--no-bundle-dylibs` while iterating locally to skip the copy/re-sign
  cost and keep depending on your Homebrew install instead.
- **Xcode project** (optional, for people who'd rather hit Cmd-B than type
  `meson`/`ninja`): `open packaging/xcode/fsv.xcodeproj`. It's a thin
  wrapper around the same Meson build — see
  [`packaging/xcode/README.md`](packaging/xcode/README.md).
- **Prebuilt binaries**: every push builds macOS (arm64) and Linux
  (x86_64) binaries; tagged releases (`v*`) attach both as `.tar.gz`
  assets on the [GitHub Releases](../../releases) page — see
  [`.github/workflows/ci.yml`](.github/workflows/ci.yml). The release
  macOS asset is a **self-contained `fsv.app`**: CI runs
  `make-bundle.sh` on the macOS runner and audits the result (`otool -l`
  across the binary and every bundled dylib) for any leftover
  `/opt/homebrew` reference, failing the job if one slips through — so
  no Homebrew install is required to run it, just unpack the tarball and
  right-click → Open (it's ad-hoc signed, not notarized, so Gatekeeper
  needs that one-time override). Building from source, per the commands
  above, still needs the Homebrew deps.

### Linux

**GTK frontend** (the legacy, OpenGL-based UI — the one Linux distributions
have packages for today):

```sh
sudo apt install libgtk-3-dev libgl1-mesa-dev libglu1-mesa-dev libepoxy-dev libcglm-dev
meson setup builddir -Dfrontend=gtk
ninja -C builddir
sudo ninja -C builddir install
```

cglm is available as of Ubuntu 20.10; on older systems, vendor it as a
Meson subproject: `mkdir -p subprojects && cd subprojects && tar xaf
/path/to/cglm-version.tar.gz && mv cglm-version cglm`.

**SDL frontend** (this port's renderer, on Linux): needs SDL3 ≥ 3.2, which
has no `apt` package on Ubuntu 22.04/24.04 as of this writing (it lands in
Ubuntu starting with 25.10) — build it from source, or install it from
your distribution if a package is available, then:

```sh
meson setup builddir -Dfrontend=sdl
ninja -C builddir
./builddir/src/sdl/fsv [dir]
```

CI builds this configuration on every push against a from-source SDL3
(see `.github/workflows/ci.yml`'s `linux-sdl` job) as a best-effort arm;
`linux-gtk` is the load-bearing Linux build.

## Controls

Gestures in the 3D viewport (ported from the original `viewport.c` mouse
handling — see [`src/sdl/input.cpp`](src/sdl/input.cpp)):

| Input | Action |
|---|---|
| Hover (no button held) | Highlight the node under the cursor; show its path in the status bar |
| Left-click, release | Select the node under the cursor and fly the camera to it ("look at") |
| Middle-drag | Dolly (zoom) the camera in/out — **in FSN mode: flight** (vertical deflection = forward/backward speed, horizontal = turn, Shift = climb/descend, release to stop) |
| Ctrl + left-drag | Revolve the camera around the current target |
| Scroll wheel | Dolly (zoom) — an addition in this port; upstream fsv has no wheel gesture, only the middle-drag |
| Double-click a directory | Toggle expand/collapse of that directory — an addition in this port; upstream fsv (and this port, before this addition) treats a double-click as just two ordinary left-clicks in a row, with no directory-activation gesture in the 3D view at all. **In FSN mode: "warp-lite"** — auto-expands it if collapsed and flies the camera down onto its pedestal, landing close and low so its file boxes fill the view (upstream fsn's own "warp", scoped down — see docs/PORTING.md's Task C4 section). Re-double-clicking an already-expanded/warped-into pedestal never collapses it (fsn's warp wasn't a toggle) — it just re-centers. Collapsing an FSN directory stays available via Escape, the context menu, or the panel's tree-row arrow |
| Double-click a file | **In FSN mode:** opens it with the system default app — upstream fsn's own "execute or view a file" gesture. First use asks for confirmation ("Open `<name>` with the system default app?"); ticking "Always allow" there skips that dialog from then on. In every other mode: no special action (same as two ordinary left-clicks) |
| Right-click | Open the context menu for the node under the cursor (Look At, Properties…, Expand/Collapse) |
| Escape | Collapse the current directory if it's expanded, otherwise collapse its parent and fly the camera there — an addition in this port; upstream fsv has no keyboard handling in the 3D view at all. Does nothing if a context menu, the open-file confirmation, or other ImGui popup is open (that gets to consume Escape first) |

Double-clicking empty space, or a file outside FSN mode, has no special
action beyond the ordinary left-click behavior above (select + fly the
camera there twice) — the directory gesture (toggle outside FSN,
warp-lite inside it) only applies to directories, and the system-open
gesture only to files in FSN mode.

Menu highlights (menu bar at the top of the window):

| Menu | Notable items |
|---|---|
| **File** | Change Root… (pick a new directory to visualize), Rescan, Quit |
| **Vis** | Switch between the three visualization modes — DiscV, MapV, TreeV |
| **View** | Toggle the docked Directory Tree & Files panel, the camera control rail, and (FSN mode only) the Overview picture-in-picture mini-map |
| **Colors** | Color nodes by type, by timestamp, or by wildcard pattern; Setup… opens the full color editor (settings persist to `~/.fsvrc`) |
| **Help** | Controls (this table, in-app), About fsv… |

## What's been done

### The FSN mode (v0.2)

A fourth visualization mode (`--fsn`, or Vis → FSN) recreating the original
SGI fsn's look and interaction from screenshots, the 1992 README, and two
SGI patents (US5555354 flight navigation, US5861885 selection spotlight) —
the original source code was never released:

- The classic landscape: gradient sky over a green ground plane
  (Display → Landscape presets), directory pedestals whose height tracks
  subtree size, file boxes colored by age, and white wires connecting
  parent to child directories.
- fsn's 7-bucket age color scheme (1 wk → > 1 yr) with the bottom legend
  bar, calibrated against period screenshots.
- Flight navigation on middle-drag, a camera control rail
  (Reset / Go back / Birds eye / Front view + Tilt/Height sliders), and
  the selection spotlight — a soft pool of light under the selected node.
- A Marks panel on the camera rail — bookmark the current node by name,
  then "Go" back to it later, rename, or delete it; persists to
  `~/.fsvrc`. A slight extension of the original: available in every
  visualization mode, not just FSN.

### The macOS / Metal port (this branch)

- Extracted a headless, GTK-free core (`libfsvcore`: scanning, geometry
  layout, camera math, color/persistence logic) behind a small
  platform-hooks header, with a `fsv-scan` CLI and unit tests exercising
  it independently of any UI.
- Replaced the OpenGL renderer with SDL3's GPU API (Metal on macOS,
  Vulkan-capable on Linux): shaders ported to GLSL 4.50, compiled offline
  to MSL and SPIR-V, and embedded directly in the binary.
- Rebuilt the entire UI in Dear ImGui — menu bar, docked directory-tree
  and file-list panels, color-setup and node-properties dialogs — with no
  GTK dependency in this frontend.
- Ported real mouse-driven object picking (color-ID offscreen readback)
  so clicking, hovering, and the context menu all resolve the actual node
  under the cursor.
- Implemented `nvstore.c` — the settings-persistence backend — for real.
  It was a complete no-op stub upstream on *both* frontends, silently
  discarding every color-setup change; it now serializes to `~/.fsvrc`,
  fixing persistence for the GTK frontend too.
- Added a scripted `--record` mode and screenshot support for
  headless/offscreen rendering, used to produce the demo video above and
  as a CI smoke test.
- Set up GitHub Actions CI: a macOS/Metal build, a Linux/GTK
  anti-regression build, a best-effort Linux/SDL build, and a release job
  that attaches macOS (arm64) and Linux (x86_64) binaries to tagged
  releases.
- Added an optional Xcode project (an external-build-system wrapper
  around the same Meson build) and a `.app`-bundling script for people
  who don't want to use the command line.
- Kept the GTK/OpenGL frontend building and working throughout — it's a
  separate `meson` target, not a fork of this one, and Linux CI builds it
  on every push.

### Inherited from jabl/fsv

- Migrated the UI from GTK+2 to GTK+3, including dropping every
  deprecated GTK/GDK API along the way.
- Modernized the OpenGL path to core-profile OpenGL 3.1 / GLSL 1.40:
  shaders and VBOs instead of the immediate-mode/display-list code the
  original SGI-era `fsv` used, with linear algebra moved onto `cglm`.
  `GL_QUADS` became `GL_TRIANGLES` throughout.
- Replaced the deprecated `GL_SELECT` picking mechanism with a modern
  color-ID offscreen-render-and-read-back technique — the same technique
  this port's Metal picking is itself based on.

### Known limitations / ideas

- **GTK4**: the GTK arm is still GTK+3; a GTK4 migration was scoped
  upstream but never completed (see the GTK/OpenGL frontend notes below).
- **DiscV**: has a few rougher visual edges than MapV/TreeV on both
  frontends — not a regression from this port, just an area that never
  got as much polish upstream.
- **Line width**: SDL_GPU has no equivalent of `glLineWidth()` on any
  backend, so every line (including the node cursor's outline) renders 1
  pixel wide on the SDL/Metal frontend — a small, deliberate cosmetic
  difference from the thicker lines the GTK/OpenGL build draws.
- **GTK frontend testing**: the GTK arm only builds a real GUI on Linux
  (macOS lacks a usable `GL/glu.h`), so its interactive behavior needs a
  Linux machine or container to exercise visually.
- **Text coverage**: filenames are rendered per Unicode codepoint and
  normalized to NFC, so accented Latin names (`café.txt`,
  `Übung_größe.txt`, `œuvre.txt`) display correctly in both the 3D
  labels and the panels — including the decomposed (NFD) form macOS
  filesystems hand back. Coverage beyond Latin/Latin-1/Latin
  Extended-A is the *font's*, not fsv's: the 3D label atlas is
  rasterized from a system monospace face (Courier New/Menlo on macOS,
  DejaVu/Liberation Mono on Linux) over those ranges only, so a CJK or
  emoji filename shows one `?` per codepoint in the 3D view. If no such
  font is found at all, fsv falls back to its built-in ASCII-only
  bitmap charset and logs it once (every non-ASCII character then
  renders as `?`).

<details>
<summary>GTK/OpenGL frontend notes (OpenGL versions and compatibility)</summary>

Gtk+3 tries to create a OpenGL 3.2 core context (since 3.16), and if that fails
it falls back to whatever legacy context it manages to create (since 3.20).
Thus one cannot assume availability of any legacy pre-3.2 API's that are
dropped in a core context. So for maximum compatibility use the oldest API's
still possible in 3.2.

For the shading language version, the latest [OpenGL core
specification](https://www.khronos.org/registry/OpenGL/specs/gl/glspec46.core.pdf)
says "The core profile of OpenGL 4.6 is also guaranteed to support all previous
versions of the OpenGL Shading Language back to version 1.40". Thus GLSL 1.40
is the minimum version. This corresponds to OpenGL 3.1. However, it also says
that "OpenGL 4.6 implementations are guaranteed to support versions 1.00, 3.00,
and 3.10 of the OpenGL ES Shading Language.". So that is also an alternative.
There's also WebGL 2.0 that roughly corresponds to OpenGL ES 3.0 & OpenGL 3.3
and also uses GLSL ES 3.0.

Fsv is a relatively simple OpenGL application and doesn't need very fancy
features. Thus, aim for OpenGL 3.1 and GLSL 1.40 in order to provide maximum
compatibility.

</details>
