# #############################################################################
# Copyright (c) 2018-present    lzutao <taolzu(at)gmail.com>
# All rights reserved.
#
# This source code is licensed under both the BSD-style license (found in the
# LICENSE file in the root directory of this source tree) and the GPLv2 (found
# in the COPYING file in the root directory of this source tree).
# #############################################################################

zstd_rootdir = '../../../..'

gen_html_includes = include_directories(join_paths(zstd_rootdir, 'programs'),
  join_paths(zstd_rootdir, 'lib'),
  join_paths(zstd_rootdir, 'lib/common'),
  join_paths(zstd_rootdir, 'contrib/gen_html'))

gen_html = executable('gen_html',
  join_paths(zstd_rootdir, 'contrib/gen_html/gen_html.cpp'),
  include_directories: gen_html_includes,
  native: true,
  install: false)

# Update zstd manual
zstd_manual_html = custom_target('zstd_manual.html',
  output : 'zstd_manual.html',
  command : [gen_html,
    zstd_version,
    join_paths(meson.current_source_dir(), zstd_rootdir, 'lib/zstd.h'),
    '@OUTPUT@'],
  install : false)
