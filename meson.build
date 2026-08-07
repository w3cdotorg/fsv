# SPDX-License-Identifier: MIT

project('fsv', 'c', version: '3.1alpha')

frontend = get_option('frontend')

# GTK is only required for the (legacy) gtk frontend, and even then only
# on a host that actually builds it (see the darwin gate in
# src/meson.build). Never `required: true` here: a genuinely GTK-less
# host must still be able to `meson setup` and build the headless core
# (libfsvcore, fsv-scan, test_scanfs) regardless of `frontend`'s value.
# The `fsv` executable target itself is gated on gtkdep_found below.
gtkdep = [dependency('gtk+-3.0', required: false),
          dependency('gdk-pixbuf-2.0', required: false),
          dependency('epoxy', required: false)]
gtkdep_found = gtkdep[0].found() and gtkdep[1].found() and gtkdep[2].found()
cglm_dep = dependency('cglm', fallback : ['cglm', 'cglm_dep'])
# debug.c (debug/) only needs <glib.h>, not GTK; declared here (rather
# than in src/meson.build, where it's used again) because debug/ is
# subdir'd first and needs it too. Previously debug/meson.build got
# glib's cflags for free as an accidental side effect of `dependencies:
# gtkdep` (gtk+-3.0's pkg-config Requires chain includes glib-2.0) —
# that broke as soon as gtkdep stopped being unconditionally required.
glibdep = dependency('glib-2.0')

# SDL3 (+ vendored Dear ImGui) is the sdl frontend's equivalent of
# gtkdep above: never `required: true` at configure time, so a host
# without SDL3 can still `meson setup` and build the headless core
# regardless of `frontend`'s value. The imgui subproject (static lib +
# `imgui_dep`, see subprojects/imgui/meson.build) is only pulled in
# when frontend == 'sdl' *and* SDL3 was actually found — mirrors
# gtkdep_found's pattern. Task 2.2 wires `imgui_dep`/`sdl3dep` into an
# actual src/sdl/ target; for now this only proves the subproject
# builds (see docs/PORTING.md, Task 2.1).
sdl3dep = dependency('sdl3', required: false)
if frontend == 'sdl' and sdl3dep.found()
  imgui_subproject = subproject('imgui')
  imgui_dep = imgui_subproject.get_variable('imgui_dep')
endif

compiler = meson.get_compiler('c')
conf = configuration_data()
conf.set_quoted('PACKAGE', meson.project_name())
conf.set_quoted('VERSION', meson.project_version())
conf.set_quoted('LOCALEDIR', get_option('datadir') + '/locale')
conf.set_quoted('DOCDIR', get_option('datadir') + '/doc/fsv')

has_gettext = compiler.has_function('gettext')
if has_gettext
  conf.set('ENABLE_NLS', 1)
endif

has_scandir = compiler.has_function('scandir')
if has_scandir
  conf.set('HAVE_SCANDIR', 1)
endif

has_file = find_program('file')
if has_file.found()
  conf.set('HAVE_FILE_COMMAND', 1)
  conf.set_quoted('FILE_COMMAND', has_file.path() + ' %s')
endif

if get_option('buildtype') == 'debug' or get_option('buildtype') == 'debugoptimized'
    add_project_arguments('-DDEBUG', language: 'c')
endif

configure_file(output : 'config.h',
               configuration : conf)

libm = compiler.find_library('m')

subdir('po')
subdir('debug')
subdir('lib')
subdir('src')
subdir('tools')
subdir('tests')

# This doesn't work with meson 0.61 and python 3.10
#rpm = import('rpm')
#rpm.generate_spec_template()
