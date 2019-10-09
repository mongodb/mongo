# #############################################################################
# Copyright (c) 2018-present     Dima Krasner <dima@dimakrasner.com>
#                                lzutao <taolzu(at)gmail.com>
# All rights reserved.
#
# This source code is licensed under both the BSD-style license (found in the
# LICENSE file in the root directory of this source tree) and the GPLv2 (found
# in the COPYING file in the root directory of this source tree).
# #############################################################################

project('zstd',
  ['c', 'cpp'],
  license: ['BSD', 'GPLv2'],
  default_options : [
    'c_std=gnu99',
    'cpp_std=c++11',
    'buildtype=release'
  ],
  version: 'DUMMY',
  meson_version: '>=0.47.0')

cc = meson.get_compiler('c')
cxx = meson.get_compiler('cpp')
pkgconfig = import('pkgconfig')
python3 = import('python').find_installation()
windows_mod = import('windows')

host_machine_os = host_machine.system()
os_windows = 'windows'
os_linux = 'linux'
os_darwin = 'darwin'
os_freebsd = 'freebsd'
os_sun = 'sunos'

cc_id = cc.get_id()
compiler_gcc = 'gcc'
compiler_clang = 'clang'
compiler_msvc = 'msvc'

zstd_version = meson.project_version()

zstd_h_file = join_paths(meson.current_source_dir(), '../../lib/zstd.h')
GetZstdLibraryVersion_py = files('GetZstdLibraryVersion.py')
r = run_command(python3, GetZstdLibraryVersion_py, zstd_h_file)
if r.returncode() == 0
  zstd_version = r.stdout().strip()
  message('Project version is now: @0@'.format(zstd_version))
else
  error('Cannot find project version in @0@'.format(zstd_h_file))
endif

zstd_libversion = zstd_version

# =============================================================================
# Installation directories
# =============================================================================

zstd_prefix = get_option('prefix')
zstd_bindir = get_option('bindir')
zstd_datadir = get_option('datadir')
zstd_mandir = get_option('mandir')
zstd_docdir = join_paths(zstd_datadir, 'doc', meson.project_name())

# =============================================================================
# Project options
# =============================================================================

# Built-in options
use_debug = get_option('debug')
buildtype = get_option('buildtype')

# Custom options
debug_level = get_option('debug_level')
legacy_level = get_option('legacy_level')
use_backtrace = get_option('backtrace')
use_static_runtime = get_option('static_runtime')

bin_programs = get_option('bin_programs')
bin_contrib = get_option('bin_contrib')
bin_tests = get_option('bin_tests')

feature_multi_thread = get_option('multi_thread')
feature_zlib = get_option('zlib')
feature_lzma = get_option('lzma')
feature_lz4 = get_option('lz4')

# =============================================================================
# Dependencies
# =============================================================================

libm_dep = cc.find_library('m', required: bin_tests)
thread_dep = dependency('threads', required: feature_multi_thread)
use_multi_thread = thread_dep.found()
# Arguments in dependency should be equivalent to those passed to pkg-config
zlib_dep = dependency('zlib', required: feature_zlib)
use_zlib = zlib_dep.found()
lzma_dep = dependency('liblzma', required: feature_lzma)
use_lzma = lzma_dep.found()
lz4_dep = dependency('liblz4', required: feature_lz4)
use_lz4 = lz4_dep.found()

# =============================================================================
# Compiler flags
# =============================================================================

add_project_arguments('-DXXH_NAMESPACE=ZSTD_', language: ['c'])

if [compiler_gcc, compiler_clang].contains(cc_id)
  common_warning_flags = [ '-Wextra', '-Wundef', '-Wshadow', '-Wcast-align', '-Wcast-qual' ]
  if cc_id == compiler_clang
    # Should use Meson's own --werror build option
    #common_warning_flags += '-Werror'
    common_warning_flags += ['-Wconversion', '-Wno-sign-conversion', '-Wdocumentation']
  endif
  cc_compile_flags = cc.get_supported_arguments(common_warning_flags + ['-Wstrict-prototypes'])
  cxx_compile_flags = cxx.get_supported_arguments(common_warning_flags)
  add_project_arguments(cc_compile_flags, language : 'c')
  add_project_arguments(cxx_compile_flags, language : 'cpp')
elif cc_id == compiler_msvc
  msvc_compile_flags = [ '/D_UNICODE', '/DUNICODE' ]
  if use_multi_thread
    msvc_compile_flags += '/MP'
  endif
  if enable_static_runtime
    msvc_compile_flags += '/MT'
  endif
  add_project_arguments(msvc_compile_flags, language: ['c', 'cpp'])
endif

# =============================================================================
# Subdirs
# =============================================================================

subdir('lib')

if bin_programs
  subdir('programs')
endif

if bin_tests
  subdir('tests')
endif

if bin_contrib
  subdir('contrib')
endif
