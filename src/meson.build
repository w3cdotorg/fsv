# SPDX-License-Identifier: MIT

# config.h is generated here, not in the root meson.build (where `conf`
# is actually assembled), and `incdir` below deliberately has no bare
# '..' entry. Both are one fix for the same bug:
#
# meson's include_directories(dir) always emits an -I flag pair for any
# relative dir: one resolved against the source tree, one against the
# build tree. A bare '..' from this file's perspective (cwd == src/)
# pairs the repo root (source side, where '..' finds this project's own
# debug/ directory) with the build root (build side, where config.h
# used to live when configure_file() was called from the root
# meson.build). There is no way to keep only the second half.
#
# That repo-root source-side entry is silently harmless for every plain
# C compile in this tree, but it is a real, load-bearing bug for the
# handful of C++ files under src/sdl/: libstdc++'s own
# <bits/stl_algobase.h> unconditionally does `#include <debug/debug.h>`
# (angle brackets), and a repo root sitting ahead of the real
# /usr/include/c++/.../debug/debug.h on the search path makes that
# resolve to *this project's* debug/debug.h instead -- which defines
# unrelated macros like g_list_alloc() (colliding with glib.h's own
# declaration once it's `#include`d later) and none of the
# __glibcxx_requires_* macros libstdc++ actually wants. Confirmed by
# reproducing both failure modes in an ubuntu:24.04 container (Task 6.2)
# and by preprocessing gpu.cpp with -E: the "note: macro ... defined
# here" pointed straight at ../debug/debug.h. This never showed up on
# macOS/Clang+libc++ (no such header, or a different search order), and
# never affects the GTK frontend (C-only, never pulls in libstdc++).
#
# Fix: config.h now lives at <builddir>/src/config.h (this file's own
# build directory), reached via the existing, harmless `.` entry's
# `-Isrc`/`-I../src` pair -- src/ has no debug/ subdirectory of its own
# to collide with anything. debug/meson.build's own `static_library()`
# picks up the new location via an explicit '../src' include dir (see
# the comment there); every other consumer of `incdir` already had
# `-Isrc`/`-I../src` for free.
configure_file(output: 'config.h', configuration: conf)

incdir = include_directories('../lib', '.')

# Headless core: filesystem scan, coloring, animation/camera math, and
# (since fsn-mode Task B1) FSV_FSN's layout. These files form libfsvcore
# and must compile with only glib-2.0/cglm/libm cflags —
# no GTK, no GL/epoxy. See docs/PORTING.md for the transitive-GTK traps
# that had to be broken in window.h for this to hold.
# (glibdep is declared in the root meson.build, before subdir('debug').)
# geometry-fsn.c is FSV_FSN's *layout* only (fsn-mode Task B1): pure
# math over the scanned tree, no gpu.h, which is what makes it core and
# not frontend -- camera.c right below it reads the layout to frame the
# landscape. Its drawing half is src/geometry-fsn-draw.c, which does call
# gpu.h and therefore belongs to the frontends, next to geometry.c.
fsvcore_src = files('scanfs.c', 'colexp.c', 'color.c', 'common.c',
                    'animation.c', 'camera.c', 'geometry-fsn.c',
                    'squarify.c')
libfsvcore = static_library('fsvcore', fsvcore_src,
  dependencies: [glibdep, cglm_dep, libm, libmisc_dep, libdebug_dep],
  include_directories: incdir)

# GTK frontend. Only built for frontend == 'gtk', only when GTK was
# actually found (never hard-required at configure time — see
# meson.build), and (for now) only on non-macOS hosts: ogl.c's GL/glu.h
# dependency doesn't build on macOS (pre-existing, out of scope for the
# Metal port). See docs/PORTING.md.
if frontend == 'gtk' and gtkdep_found and host_machine.system() != 'darwin'
  gnome = import('gnome')

  gr = gnome.compile_resources('gr', 'fsv-gresource.xml')

  srcs = ['about.c', 'animation.c', 'callbacks.c', 'camera.c', 'colexp.c',
    'color.c', 'common.c', 'dialog.c', 'dirtree.c', 'filelist.c',
    'fontatlas.c', 'fsv.c',
    'geometry.c', 'geometry-fsn.c', 'geometry-fsn-draw.c',
    'gui.c', 'ogl.c', 'ogl-gpu-compat.c', 'scanfs.c',
    'tmaptext.c',
    'viewport.c', 'window.c']
  executable('fsv', sources: [srcs, gr],
    dependencies : [libmisc_dep, libdebug_dep, gtkdep, libm, cglm_dep],
    include_directories: incdir)
endif
