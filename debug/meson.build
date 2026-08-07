# SPDX-License-Identifier: MIT

sources = 'debug.c'
# debug.c only uses glib (GList/GNode wrappers, g_log_set_fatal_mask);
# it calls no GTK function. It used to list `dependencies: gtkdep`,
# which happened to work only because gtk+-3.0's pkg-config Requires
# chain drags in glib-2.0's cflags — broken once gtkdep is genuinely
# absent. Depend on glib-2.0 directly instead.
libdebug = static_library('debug', sources, include_directories: '..',
  dependencies: glibdep)
libdebug_dep = declare_dependency(include_directories: '.', link_with: libdebug)
