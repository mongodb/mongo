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

zstd_programs_sources = [join_paths(zstd_rootdir, 'programs/zstdcli.c'),
  join_paths(zstd_rootdir, 'programs/util.c'),
  join_paths(zstd_rootdir, 'programs/timefn.c'),
  join_paths(zstd_rootdir, 'programs/fileio.c'),
  join_paths(zstd_rootdir, 'programs/benchfn.c'),
  join_paths(zstd_rootdir, 'programs/benchzstd.c'),
  join_paths(zstd_rootdir, 'programs/datagen.c'),
  join_paths(zstd_rootdir, 'programs/dibio.c')]

zstd_c_args = libzstd_debug_cflags
if use_multi_thread
  zstd_c_args += [ '-DZSTD_MULTITHREAD' ]
endif

zstd_deps = [ libzstd_dep ]
if use_zlib
  zstd_deps += [ zlib_dep ]
  zstd_c_args += [ '-DZSTD_GZCOMPRESS', '-DZSTD_GZDECOMPRESS' ]
endif

if use_lzma
  zstd_deps += [ lzma_dep ]
  zstd_c_args += [ '-DZSTD_LZMACOMPRESS', '-DZSTD_LZMADECOMPRESS' ]
endif

if use_lz4
  zstd_deps += [ lz4_dep ]
  zstd_c_args += [ '-DZSTD_LZ4COMPRESS', '-DZSTD_LZ4DECOMPRESS' ]
endif

export_dynamic_on_windows = false
# explicit backtrace enable/disable for Linux & Darwin
if not use_backtrace
  zstd_c_args += '-DBACKTRACE_ENABLE=0'
elif use_debug and host_machine_os == os_windows  # MinGW target
  zstd_c_args += '-DBACKTRACE_ENABLE=1'
  export_dynamic_on_windows = true
endif

if cc_id == compiler_msvc
  if default_library_type != 'static'
    zstd_programs_sources += [windows_mod.compile_resources(
      join_paths(zstd_rootdir, 'build/VS2010/zstd/zstd.rc'))]
  endif
endif

zstd = executable('zstd',
  zstd_programs_sources,
  c_args: zstd_c_args,
  dependencies: zstd_deps,
  export_dynamic: export_dynamic_on_windows, # Since Meson 0.45.0
  install: true)

zstd_frugal_sources = [join_paths(zstd_rootdir, 'programs/zstdcli.c'),
  join_paths(zstd_rootdir, 'programs/timefn.c'),
  join_paths(zstd_rootdir, 'programs/util.c'),
  join_paths(zstd_rootdir, 'programs/fileio.c')]

# Minimal target, with only zstd compression and decompression.
# No bench. No legacy.
executable('zstd-frugal',
  zstd_frugal_sources,
  dependencies: libzstd_dep,
  c_args: [ '-DZSTD_NOBENCH', '-DZSTD_NODICT' ],
  install: true)

install_data(join_paths(zstd_rootdir, 'programs/zstdgrep'),
  join_paths(zstd_rootdir, 'programs/zstdless'),
  install_dir: zstd_bindir)

# =============================================================================
# Programs and manpages installing
# =============================================================================

install_man(join_paths(zstd_rootdir, 'programs/zstd.1'),
  join_paths(zstd_rootdir, 'programs/zstdgrep.1'),
  join_paths(zstd_rootdir, 'programs/zstdless.1'))

InstallSymlink_py = '../InstallSymlink.py'
zstd_man1_dir = join_paths(zstd_mandir, 'man1')
bin_EXT = host_machine_os == os_windows ? '.exe' : ''
man1_EXT = meson.version().version_compare('>=0.49.0') ? '.1' : '.1.gz'

foreach f : ['zstdcat', 'unzstd']
  meson.add_install_script(InstallSymlink_py, 'zstd' + bin_EXT, f + bin_EXT, zstd_bindir)
  meson.add_install_script(InstallSymlink_py, 'zstd' + man1_EXT, f + man1_EXT, zstd_man1_dir)
endforeach

if use_multi_thread
  meson.add_install_script(InstallSymlink_py, 'zstd' + bin_EXT, 'zstdmt' + bin_EXT, zstd_bindir)
  meson.add_install_script(InstallSymlink_py, 'zstd' + man1_EXT, 'zstdmt' + man1_EXT, zstd_man1_dir)
endif
