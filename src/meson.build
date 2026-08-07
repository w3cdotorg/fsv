# SPDX-License-Identifier: MIT

incdir = include_directories('..', '../lib', '.')

# Headless core: filesystem scan, coloring, animation/camera math.
# These six files (plus geometry.c once its GL calls are gone, in M3)
# form libfsvcore and must compile with only glib-2.0/cglm/libm cflags —
# no GTK, no GL/epoxy. See docs/PORTING.md for the transitive-GTK traps
# that had to be broken in window.h for this to hold.
# (glibdep is declared in the root meson.build, before subdir('debug').)
fsvcore_src = files('scanfs.c', 'colexp.c', 'color.c', 'common.c',
                    'animation.c', 'camera.c')
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
    'color.c', 'common.c', 'dialog.c', 'dirtree.c', 'filelist.c', 'fsv.c',
    'geometry.c', 'gui.c', 'ogl.c', 'scanfs.c', 'tmaptext.c',
    'viewport.c', 'window.c']
  executable('fsv', sources: [srcs, gr],
    dependencies : [libmisc_dep, libdebug_dep, gtkdep, libm, cglm_dep],
    include_directories: incdir)
endif
