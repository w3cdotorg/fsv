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
#   packaging/macos/make-bundle.sh [builddir-or-binary] [output.app]
#
#   builddir-or-binary  Either a meson build directory that has already
#               been built (meson setup <builddir> && ninja -C <builddir>),
#               or a direct path to an already-built fsv binary -- the
#               latter is what a release tarball ships (the binary sits
#               flat next to this script, not inside a meson builddir).
#               If it names a regular file, it's used as-is; if it
#               names a directory, "<dir>/src/sdl/fsv" is looked up
#               inside it. Defaults to "builddir" at the repo root; if
#               that does not contain a built binary, also tries
#               "builddir-xcode" (the directory the Xcode
#               external-build target uses).
#   output.app  Where to write the bundle. Defaults to "fsv.app" at the
#               repo root.
#
# Examples:
#   meson setup builddir && ninja -C builddir
#   packaging/macos/make-bundle.sh
#   open fsv.app
#
#   # From an extracted release tarball (binary flat next to this script):
#   ./make-bundle.sh ./fsv fsv.app
#   open fsv.app

set -eu

script_dir=$(cd "$(dirname "$0")" && pwd)
repo_root=$(cd "$script_dir/../.." && pwd)

binary_arg="${1:-}"
app_path="${2:-$repo_root/fsv.app}"

find_binary() {
	candidate="$1/src/sdl/fsv"
	if [ -x "$candidate" ]; then
		printf '%s\n' "$candidate"
		return 0
	fi
	return 1
}

if [ -n "$binary_arg" ] && [ -f "$binary_arg" ]; then
	# A direct, already-built binary path (release-tarball layout) --
	# used as-is, no meson builddir to look inside.
	if [ ! -x "$binary_arg" ]; then
		echo "error: $binary_arg exists but is not executable" >&2
		exit 1
	fi
	binary="$binary_arg"
elif [ -n "$binary_arg" ]; then
	binary=$(find_binary "$binary_arg") || {
		echo "error: no built binary at $binary_arg/src/sdl/fsv" >&2
		echo "       run: meson setup $binary_arg && ninja -C $binary_arg" >&2
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
