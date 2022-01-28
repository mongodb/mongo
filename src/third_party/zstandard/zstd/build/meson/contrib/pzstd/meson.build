# #############################################################################
# Copyright (c) 2018-present    lzutao <taolzu(at)gmail.com>
# All rights reserved.
#
# This source code is licensed under both the BSD-style license (found in the
# LICENSE file in the root directory of this source tree) and the GPLv2 (found
# in the COPYING file in the root directory of this source tree).
# #############################################################################

zstd_rootdir = '../../../..'

pzstd_includes = include_directories(join_paths(zstd_rootdir, 'programs'),
  join_paths(zstd_rootdir, 'contrib/pzstd'))
pzstd_sources = [join_paths(zstd_rootdir, 'programs/util.c'),
  join_paths(zstd_rootdir, 'contrib/pzstd/main.cpp'),
  join_paths(zstd_rootdir, 'contrib/pzstd/Options.cpp'),
  join_paths(zstd_rootdir, 'contrib/pzstd/Pzstd.cpp'),
  join_paths(zstd_rootdir, 'contrib/pzstd/SkippableFrame.cpp')]
pzstd = executable('pzstd',
  pzstd_sources,
  cpp_args: [ '-DNDEBUG', '-Wno-shadow', '-Wno-deprecated-declarations' ],
  include_directories: pzstd_includes,
  dependencies: [ libzstd_dep, thread_dep ],
  install: true)
