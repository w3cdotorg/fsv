#!/bin/sh
# Assemble fsv.app from an existing Meson build of the SDL/Metal frontend.
#
# Meson is the single source of truth for the build; this script only
# copies its output into a standard macOS .app bundle layout, bundles
# its runtime dylib dependencies so the .app is relocatable without a
# `brew install`, and ad-hoc code-signs it (no paid Apple Developer
# account, no team, no provisioning profile -- "Sign to Run Locally"
# equivalent for a CLI workflow). It does not compile anything itself.
#
# Usage:
#   packaging/macos/make-bundle.sh [options] [builddir-or-binary] [output.app]
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
# Options:
#   --no-bundle-dylibs
#               Skip dylib bundling (see below). The resulting .app
#               still requires its runtime libraries (SDL3, glib, and
#               whatever else the binary was linked against) to be
#               installed on the machine that runs it, e.g. via
#               `brew install sdl3 glib`. This is a dev convenience
#               for iterating locally against a Homebrew toolchain
#               without paying the copy/re-sign cost on every rebuild
#               -- release bundles should NOT use this flag.
#
# Dylib bundling (default, on by default):
#   The script computes the transitive closure of non-system dylibs
#   the binary (and each bundled dylib, recursively) depends on, by
#   parsing `otool -L` -- no hard-coded library list. "Non-system"
#   means anything other than /usr/lib/*, /System/*, or a dependency
#   that's already expressed as @rpath/@executable_path/@loader_path.
#   Each discovered dylib is copied (symlinks resolved) into
#   Contents/Frameworks, its own install name (id) and its references
#   to other bundled dylibs are rewritten to @rpath/<basename> with
#   install_name_tool, the main binary's references are rewritten the
#   same way, an @executable_path/../Frameworks rpath is added to the
#   binary, and everything is re-signed ad-hoc inside-out (Frameworks
#   dylibs, then the binary, then the bundle) since install_name_tool
#   invalidates existing signatures.
#
# Examples:
#   meson setup builddir && ninja -C builddir
#   packaging/macos/make-bundle.sh
#   open fsv.app
#
#   # From an extracted release tarball (binary flat next to this script):
#   ./make-bundle.sh ./fsv fsv.app
#   open fsv.app
#
#   # Dev iteration without the dylib-bundling/re-signing cost:
#   ./make-bundle.sh --no-bundle-dylibs

set -eu

script_dir=$(cd "$(dirname "$0")" && pwd)
repo_root=$(cd "$script_dir/../.." && pwd)

bundle_dylibs=1
positional_count=0
binary_arg=""
app_path_arg=""
for arg in "$@"; do
	case "$arg" in
		--no-bundle-dylibs)
			bundle_dylibs=0
			;;
		--)
			;;
		-*)
			echo "error: unknown option: $arg" >&2
			exit 1
			;;
		*)
			positional_count=$((positional_count + 1))
			if [ "$positional_count" -eq 1 ]; then
				binary_arg="$arg"
			elif [ "$positional_count" -eq 2 ]; then
				app_path_arg="$arg"
			else
				echo "error: too many arguments: $arg" >&2
				exit 1
			fi
			;;
	esac
done
app_path="${app_path_arg:-$repo_root/fsv.app}"

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

bin_path="$app_path/Contents/MacOS/fsv"

if [ "$bundle_dylibs" -eq 1 ]; then
	echo "Computing transitive dylib closure..."

	tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/fsv-bundle.XXXXXX")
	trap 'rm -rf "$tmpdir"' EXIT

	closure_file="$tmpdir/closure" # discovered non-system deps, original absolute paths, one per line
	queue_file="$tmpdir/queue"     # deps discovered but not yet scanned themselves
	: > "$closure_file"
	: > "$queue_file"

	# Prints the non-self dependency paths of a Mach-O file, one per
	# line. otool -L lists a dylib's own install name (id) first --
	# pass is_dylib=1 to skip that line; the main executable has no
	# such self-entry, so is_dylib=0 keeps every line.
	scan_deps() {
		file="$1"
		is_dylib="$2"
		start=2
		if [ "$is_dylib" -eq 1 ]; then
			start=3
		fi
		otool -L "$file" | tail -n +"$start" | sed -e 's/^[[:space:]]*//' -e 's/ (compatibility.*$//'
	}

	# A dependency is part of the closure unless it's a system library
	# or already expressed relative to the bundle/loader.
	is_system_or_bundle_relative() {
		case "$1" in
			/usr/lib/*|/System/*|@rpath/*|@executable_path/*|@loader_path/*)
				return 0
				;;
			*)
				return 1
				;;
		esac
	}

	enqueue_dep() {
		dep="$1"
		if is_system_or_bundle_relative "$dep"; then
			return 0
		fi
		if grep -qxF "$dep" "$closure_file" 2>/dev/null; then
			return 0
		fi
		printf '%s\n' "$dep" >> "$closure_file"
		printf '%s\n' "$dep" >> "$queue_file"
	}

	scan_deps "$bin_path" 0 | while IFS= read -r dep; do
		enqueue_dep "$dep"
	done

	while [ -s "$queue_file" ]; do
		current=$(head -n 1 "$queue_file")
		tail -n +2 "$queue_file" > "$queue_file.next"
		mv "$queue_file.next" "$queue_file"
		scan_deps "$current" 1 | while IFS= read -r dep; do
			enqueue_dep "$dep"
		done
	done

	if [ -s "$closure_file" ]; then
		echo "Dylib closure:"
		sed 's/^/  /' "$closure_file"
	else
		echo "Dylib closure is empty (binary only links system libraries)."
	fi

	frameworks_dir="$app_path/Contents/Frameworks"
	mkdir -p "$frameworks_dir"

	# Copy (resolving symlinks) every closure member into Frameworks/.
	while IFS= read -r dep; do
		base=$(basename "$dep")
		cp -L "$dep" "$frameworks_dir/$base"
		chmod u+w "$frameworks_dir/$base"
	done < "$closure_file"

	# Rewrite each copied dylib: its own id becomes @rpath/<basename>,
	# and each of its own (originally discovered) non-system deps is
	# retargeted to @rpath/<that dep's basename>.
	while IFS= read -r dep; do
		base=$(basename "$dep")
		target="$frameworks_dir/$base"
		install_name_tool -id "@rpath/$base" "$target"
		scan_deps "$dep" 1 | while IFS= read -r subdep; do
			if is_system_or_bundle_relative "$subdep"; then
				continue
			fi
			subbase=$(basename "$subdep")
			install_name_tool -change "$subdep" "@rpath/$subbase" "$target"
		done
	done < "$closure_file"

	# Rewrite the main binary's references to each closure member, and
	# point it at Contents/Frameworks via rpath.
	while IFS= read -r dep; do
		base=$(basename "$dep")
		install_name_tool -change "$dep" "@rpath/$base" "$bin_path"
	done < "$closure_file"

	if ! add_rpath_out=$(install_name_tool -add_rpath "@executable_path/../Frameworks" "$bin_path" 2>&1); then
		case "$add_rpath_out" in
			*"would duplicate path"*)
				: # already present (e.g. binary was bundled before) -- fine
				;;
			*)
				echo "$add_rpath_out" >&2
				exit 1
				;;
		esac
	fi

	# Meson/pkg-config also bake link-time -rpath flags for dependency
	# lib dirs (e.g. glib's own Cellar path, gettext's opt path, cglm's
	# Cellar path) into LC_RPATH commands, independent of the LC_LOAD_DYLIB
	# entries scanned above. otool -L never shows these, but they are
	# still absolute non-bundle paths, and -- worse -- on a machine
	# where those Homebrew paths happen to exist, dyld resolves an
	# @rpath/ load command against the FIRST matching LC_RPATH entry, so
	# a stray Homebrew rpath ahead of ours would make the binary quietly
	# load the system's Homebrew dylib instead of the bundled copy,
	# which a plain launch test would never catch. Strip every absolute
	# rpath (anything not already @executable_path/@loader_path/@rpath)
	# so only the Frameworks-relative one we just added remains.
	strip_stray_rpaths() {
		target="$1"
		otool -l "$target" | awk '
			/cmd LC_RPATH/ { state=1; next }
			state==1 { state=2; next }
			state==2 {
				line=$0
				sub(/^[ \t]*path[ \t]*/, "", line)
				sub(/ \(offset [0-9]+\)$/, "", line)
				print line
				state=0
			}
		' | while IFS= read -r rp; do
			case "$rp" in
				/*)
					install_name_tool -delete_rpath "$rp" "$target"
					;;
			esac
		done
	}
	strip_stray_rpaths "$bin_path"
	while IFS= read -r dep; do
		base=$(basename "$dep")
		strip_stray_rpaths "$frameworks_dir/$base"
	done < "$closure_file"

	# install_name_tool invalidates any existing signature, and it must
	# be redone after ALL rewrites are complete. Sign inside-out:
	# each bundled dylib, then the main binary, then the bundle itself.
	if [ -s "$closure_file" ]; then
		while IFS= read -r dep; do
			base=$(basename "$dep")
			codesign --force --sign - "$frameworks_dir/$base"
		done < "$closure_file"
	fi
	codesign --force --sign - "$bin_path"
	codesign --force --sign - "$app_path"

	rm -rf "$tmpdir"
	trap - EXIT
else
	echo "Skipping dylib bundling (--no-bundle-dylibs); .app requires runtime libs on the host."
	# Ad-hoc sign (no team, no identity) so Gatekeeper's local "Sign to
	# Run Locally" equivalent is satisfied for a CLI-built bundle. This
	# does NOT satisfy notarization/spctl for distribution to other
	# machines. --deep is fine here since there's nothing nested under
	# the bundle to sign independently.
	codesign --force --deep --sign - "$app_path"
fi

echo "Done. Launch with: open \"$app_path\""
