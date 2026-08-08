# SPDX-License-Identifier: MIT

# stb_truetype.c is the single TU that compiles lib/stb_truetype.h's
# implementation, for src/fontatlas.c's glyph rasterizer (post-port
# UTF-8 work). Lives here, next to nvstore.c, because it is
# frontend-agnostic third-party C and both frontends link libmisc.
sources = ['nvstore.c', 'stb_truetype.c']
libmisc = static_library('misc', sources)
libmisc_dep = declare_dependency(include_directories: '.', link_with: libmisc)
