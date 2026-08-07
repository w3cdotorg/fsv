# SPDX-License-Identifier: MIT

sources = 'debug.c'
# debug.c only uses glib (GList/GNode wrappers, g_log_set_fatal_mask);
# it calls no GTK function. It used to list `dependencies: gtkdep`,
# which happened to work only because gtk+-3.0's pkg-config Requires
# chain drags in glib-2.0's cflags — broken once gtkdep is genuinely
# absent. Depend on glib-2.0 directly instead.
# '../src' is required since Task 6.2: config.h now generates into
# <builddir>/src/config.h, not the build root -- see the long comment
# above `incdir` in src/meson.build for why. No bare '..' here: debug.c
# (checked via its #include list) needs nothing else from the repo
# root, and the whole point of that fix was to stop putting the repo
# root on any compile's include path when nothing actually needs it.
libdebug = static_library('debug', sources, include_directories: ['../src'],
  dependencies: glibdep)
libdebug_dep = declare_dependency(include_directories: '.', link_with: libdebug)
