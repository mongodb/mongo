# #############################################################################
# Copyright (c) 2018-present     Dima Krasner <dima@dimakrasner.com>
#                                lzutao <taolzu(at)gmail.com>
# All rights reserved.
#
# This source code is licensed under both the BSD-style license (found in the
# LICENSE file in the root directory of this source tree) and the GPLv2 (found
# in the COPYING file in the root directory of this source tree).
# #############################################################################

zstd_rootdir = '../../..'

libzstd_includes = [include_directories(join_paths(zstd_rootdir,'lib'),
  join_paths(zstd_rootdir, 'lib/common'),
  join_paths(zstd_rootdir, 'lib/compress'),
  join_paths(zstd_rootdir, 'lib/decompress'),
  join_paths(zstd_rootdir, 'lib/dictBuilder'))]

libzstd_sources = [join_paths(zstd_rootdir, 'lib/common/entropy_common.c'),
  join_paths(zstd_rootdir, 'lib/common/fse_decompress.c'),
  join_paths(zstd_rootdir, 'lib/common/threading.c'),
  join_paths(zstd_rootdir, 'lib/common/pool.c'),
  join_paths(zstd_rootdir, 'lib/common/zstd_common.c'),
  join_paths(zstd_rootdir, 'lib/common/error_private.c'),
  join_paths(zstd_rootdir, 'lib/common/xxhash.c'),
  join_paths(zstd_rootdir, 'lib/compress/hist.c'),
  join_paths(zstd_rootdir, 'lib/compress/fse_compress.c'),
  join_paths(zstd_rootdir, 'lib/compress/huf_compress.c'),
  join_paths(zstd_rootdir, 'lib/compress/zstd_compress.c'),
  join_paths(zstd_rootdir, 'lib/compress/zstd_compress_literals.c'),
  join_paths(zstd_rootdir, 'lib/compress/zstd_compress_sequences.c'),
  join_paths(zstd_rootdir, 'lib/compress/zstd_compress_superblock.c'),
  join_paths(zstd_rootdir, 'lib/compress/zstdmt_compress.c'),
  join_paths(zstd_rootdir, 'lib/compress/zstd_fast.c'),
  join_paths(zstd_rootdir, 'lib/compress/zstd_double_fast.c'),
  join_paths(zstd_rootdir, 'lib/compress/zstd_lazy.c'),
  join_paths(zstd_rootdir, 'lib/compress/zstd_opt.c'),
  join_paths(zstd_rootdir, 'lib/compress/zstd_ldm.c'),
  join_paths(zstd_rootdir, 'lib/decompress/huf_decompress.c'),
  join_paths(zstd_rootdir, 'lib/decompress/zstd_decompress.c'),
  join_paths(zstd_rootdir, 'lib/decompress/zstd_decompress_block.c'),
  join_paths(zstd_rootdir, 'lib/decompress/zstd_ddict.c'),
  join_paths(zstd_rootdir, 'lib/dictBuilder/cover.c'),
  join_paths(zstd_rootdir, 'lib/dictBuilder/fastcover.c'),
  join_paths(zstd_rootdir, 'lib/dictBuilder/divsufsort.c'),
  join_paths(zstd_rootdir, 'lib/dictBuilder/zdict.c')]

# really we need anything that defines __GNUC__ as that is what ZSTD_ASM_SUPPORTED is gated on
# but these are the two compilers that are supported in tree and actually handle this correctly
# Otherwise, explicitly disable assmebly.
if [compiler_gcc, compiler_clang].contains(cc_id)
  libzstd_sources += join_paths(zstd_rootdir, 'lib/decompress/huf_decompress_amd64.S')
else
  add_project_arguments('-DZSTD_DISABLE_ASM', language: 'c')
endif

# Explicit define legacy support
add_project_arguments('-DZSTD_LEGACY_SUPPORT=@0@'.format(legacy_level),
  language: 'c')

if legacy_level == 0
  message('Legacy support: DISABLED')
else
  # See ZSTD_LEGACY_SUPPORT of lib/README.md
  message('Enable legacy support back to version 0.@0@'.format(legacy_level))

  libzstd_includes += [ include_directories(join_paths(zstd_rootdir, 'lib/legacy')) ]
  foreach i : [1, 2, 3, 4, 5, 6, 7]
    if legacy_level <= i
      libzstd_sources += join_paths(zstd_rootdir, 'lib/legacy/zstd_v0@0@.c'.format(i))
    endif
  endforeach
endif

libzstd_deps = []
if use_multi_thread
  message('Enable multi-threading support')
  add_project_arguments('-DZSTD_MULTITHREAD', language: 'c')
  libzstd_deps = [ thread_dep ]
endif

libzstd_c_args = []
if cc_id == compiler_msvc
  if default_library_type != 'static'
    libzstd_sources += [windows_mod.compile_resources(
      join_paths(zstd_rootdir, 'build/VS2010/libzstd-dll/libzstd-dll.rc'))]
    libzstd_c_args += ['-DZSTD_DLL_EXPORT=1',
      '-DZSTD_HEAPMODE=0',
      '-D_CONSOLE',
      '-D_CRT_SECURE_NO_WARNINGS']
  else
    libzstd_c_args += ['-DZSTD_HEAPMODE=0',
      '-D_CRT_SECURE_NO_WARNINGS']
  endif
endif

mingw_ansi_stdio_flags = []
if host_machine_os == os_windows and cc_id == compiler_gcc
  mingw_ansi_stdio_flags = [ '-D__USE_MINGW_ANSI_STDIO' ]
endif
libzstd_c_args += mingw_ansi_stdio_flags

libzstd_debug_cflags = []
if use_debug
  libzstd_c_args += '-DDEBUGLEVEL=@0@'.format(debug_level)
  if cc_id == compiler_gcc or cc_id == compiler_clang
    libzstd_debug_cflags = ['-Wstrict-aliasing=1', '-Wswitch-enum',
      '-Wdeclaration-after-statement', '-Wstrict-prototypes',
      '-Wundef', '-Wpointer-arith', '-Wvla',
      '-Wformat=2', '-Winit-self', '-Wfloat-equal', '-Wwrite-strings',
      '-Wredundant-decls', '-Wmissing-prototypes', '-Wc++-compat']
  endif
endif
libzstd_c_args += cc.get_supported_arguments(libzstd_debug_cflags)

libzstd = library('zstd',
  libzstd_sources,
  include_directories: libzstd_includes,
  c_args: libzstd_c_args,
  gnu_symbol_visibility: 'hidden',
  dependencies: libzstd_deps,
  install: true,
  version: zstd_libversion)

libzstd_dep = declare_dependency(link_with: libzstd,
  include_directories: libzstd_includes)

pkgconfig.generate(libzstd,
  name: 'libzstd',
  filebase: 'libzstd',
  description: 'fast lossless compression algorithm library',
  version: zstd_libversion,
  url: 'http://www.zstd.net/')

install_headers(join_paths(zstd_rootdir, 'lib/zstd.h'),
  join_paths(zstd_rootdir, 'lib/zdict.h'),
  join_paths(zstd_rootdir, 'lib/zstd_errors.h'))
