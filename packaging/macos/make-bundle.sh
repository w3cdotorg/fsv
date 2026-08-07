#!/bin/sh
# Assemble fsv.app from an existing Meson build of the SDL/Metal frontend.
#
# Meson is the single source of truth for the build; this script only
# copies its output into a standard macOS .app bundle layout and ad-hoc
# code-signs it (no paid Apple Developer account, no team, no
# provisioning profile -- "Sign to Run Locally" equivalent for a CLI
# workflow). It does not compile anything itself.
#
# Usage:
#   packaging/macos/make-bundle.sh [builddir] [output.app]
#
#   builddir    Path to a meson build directory that has already been
#               built (meson setup <builddir> && ninja -C <builddir>).
#               Defaults to "builddir" at the repo root; if that does
#               not contain a built binary, also tries "builddir-xcode"
#               (the directory the Xcode external-build target uses).
#   output.app  Where to write the bundle. Defaults to "fsv.app" at the
#               repo root.
#
# Example:
#   meson setup builddir && ninja -C builddir
#   packaging/macos/make-bundle.sh
#   open fsv.app

set -eu

script_dir=$(cd "$(dirname "$0")" && pwd)
repo_root=$(cd "$script_dir/../.." && pwd)

builddir_arg="${1:-}"
app_path="${2:-$repo_root/fsv.app}"

find_binary() {
	candidate="$1/src/sdl/fsv"
	if [ -x "$candidate" ]; then
		printf '%s\n' "$candidate"
		return 0
	fi
	return 1
}

if [ -n "$builddir_arg" ]; then
	binary=$(find_binary "$builddir_arg") || {
		echo "error: no built binary at $builddir_arg/src/sdl/fsv" >&2
		echo "       run: meson setup $builddir_arg && ninja -C $builddir_arg" >&2
		exit 1
	}
else
	binary=""
	for d in "$repo_root/builddir" "$repo_root/builddir-xcode"; do
		if binary=$(find_binary "$d"); then
			break
		fi
		binary=""
	done
	if [ -z "$binary" ]; then
		echo "error: no built binary found under builddir/ or builddir-xcode/" >&2
		echo "       run: meson setup builddir && ninja -C builddir" >&2
		exit 1
	fi
fi

echo "Bundling $binary -> $app_path"

rm -rf "$app_path"
mkdir -p "$app_path/Contents/MacOS"
mkdir -p "$app_path/Contents/Resources"

cp "$binary" "$app_path/Contents/MacOS/fsv"
cp "$script_dir/Info.plist" "$app_path/Contents/Info.plist"

# No fsv.icns is checked in (the only source icon in the tree is the
# legacy GTK src/xmaps/fsv-icon.xpm, an XPM, which iconutil/sips cannot
# turn into an .icns without a hand-drawn multi-resolution PNG set) --
# skipped per this task's own scope. If one is ever added at
# packaging/macos/fsv.icns, drop the line below and reference it via
# CFBundleIconFile in Info.plist.
if [ -f "$script_dir/fsv.icns" ]; then
	cp "$script_dir/fsv.icns" "$app_path/Contents/Resources/fsv.icns"
fi

# Ad-hoc sign (no team, no identity) so Gatekeeper's local "Sign to Run
# Locally" equivalent is satisfied for a CLI-built bundle. This does
# NOT satisfy notarization/spctl for distribution to other machines.
codesign --force --deep --sign - "$app_path"

echo "Done. Launch with: open \"$app_path\""
