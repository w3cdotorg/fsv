# SPDX-License-Identifier: MIT

sources = 'debug.c'
# debug.c only uses glib (GList/GNode wrappers, g_log_set_fatal_mask);
# it calls no GTK function. It used to list `dependencies: gtkdep`,
# which happened to work only because gtk+-3.0's pkg-config Requires
# chain drags in glib-2.0's cflags — broken once gtkdep is genuinely
# absent. Depend on glib-2.0 directly instead.
# '../src' (not just '..') is required since Task 6.2: config.h now
# generates into <builddir>/src/config.h, not the build root -- see the
# long comment above `incdir` in src/meson.build for why. '..' itself
# is kept only in case debug.c ever needs another repo-root header;
# harmless here since debug.c is plain C (the bug that motivated moving
# config.h is specific to libstdc++/C++ compiles).
libdebug = static_library('debug', sources, include_directories: ['..', '../src'],
  dependencies: glibdep)
libdebug_dep = declare_dependency(include_directories: '.', link_with: libdebug)
