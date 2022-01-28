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

tests_supported_oses = [os_linux, 'gnu/kfreebsd', os_darwin, 'gnu', 'openbsd',
  os_freebsd, 'netbsd', 'dragonfly', os_sun]

# =============================================================================
# Test flags
# =============================================================================

FUZZER_FLAGS = ['--no-big-tests']
FUZZERTEST = '-T200s'
ZSTREAM_TESTTIME = '-T90s'
DECODECORPUS_TESTTIME = '-T30'
ZSTDRTTEST = ['--test-large-data']

# =============================================================================
# Executables
# =============================================================================

test_includes = [ include_directories(join_paths(zstd_rootdir, 'programs')) ]

testcommon_sources = [join_paths(zstd_rootdir, 'programs/datagen.c'),
  join_paths(zstd_rootdir, 'programs/util.c'),
  join_paths(zstd_rootdir, 'programs/timefn.c'),
  join_paths(zstd_rootdir, 'programs/benchfn.c'),
  join_paths(zstd_rootdir, 'programs/benchzstd.c')]

testcommon = static_library('testcommon',
  testcommon_sources,
  # needed due to use of private symbol + -fvisibility=hidden
  objects: libzstd.extract_all_objects(recursive: false))

testcommon_dep = declare_dependency(link_with: testcommon,
  dependencies: libzstd_deps,
  include_directories: libzstd_includes)

datagen_sources = [join_paths(zstd_rootdir, 'tests/datagencli.c')]
datagen = executable('datagen',
  datagen_sources,
  c_args: [ '-DNDEBUG' ],
  include_directories: test_includes,
  dependencies: testcommon_dep,
  install: false)

fullbench_sources = [join_paths(zstd_rootdir, 'tests/fullbench.c')]
fullbench = executable('fullbench',
  fullbench_sources,
  include_directories: test_includes,
  dependencies: testcommon_dep,
  install: false)

fuzzer_sources = [join_paths(zstd_rootdir, 'tests/fuzzer.c')]
fuzzer = executable('fuzzer',
  fuzzer_sources,
  include_directories: test_includes,
  dependencies: [ testcommon_dep, thread_dep ],
  install: false)

zstreamtest_sources = [join_paths(zstd_rootdir, 'tests/seqgen.c'),
  join_paths(zstd_rootdir, 'tests/zstreamtest.c')]
zstreamtest = executable('zstreamtest',
  zstreamtest_sources,
  include_directories: test_includes,
  dependencies: testcommon_dep,
  install: false)

paramgrill_sources = [join_paths(zstd_rootdir, 'tests/paramgrill.c')]
paramgrill = executable('paramgrill',
  paramgrill_sources,
  include_directories: test_includes,
  dependencies: [ testcommon_dep, libm_dep ],
  install: false)

roundTripCrash_sources = [join_paths(zstd_rootdir, 'tests/roundTripCrash.c')]
roundTripCrash = executable('roundTripCrash',
  roundTripCrash_sources,
  dependencies: [ testcommon_dep ],
  install: false)

longmatch_sources = [join_paths(zstd_rootdir, 'tests/longmatch.c')]
longmatch = executable('longmatch',
  longmatch_sources,
  dependencies: [ libzstd_dep ],
  install: false)

invalidDictionaries_sources = [join_paths(zstd_rootdir, 'tests/invalidDictionaries.c')]
invalidDictionaries = executable('invalidDictionaries',
  invalidDictionaries_sources,
  dependencies: [ libzstd_dep ],
  install: false)

if 0 < legacy_level and legacy_level <= 4
  legacy_sources = [join_paths(zstd_rootdir, 'tests/legacy.c')]
  legacy = executable('legacy',
    legacy_sources,
    # Use -Dlegacy_level build option to control it
    #c_args: '-DZSTD_LEGACY_SUPPORT=4',
    dependencies: [ libzstd_dep ],
    install: false)
endif

decodecorpus_sources = [join_paths(zstd_rootdir, 'tests/decodecorpus.c')]
decodecorpus = executable('decodecorpus',
  decodecorpus_sources,
  include_directories: test_includes,
  dependencies: [ testcommon_dep, libm_dep ],
  install: false)

poolTests_sources = [join_paths(zstd_rootdir, 'tests/poolTests.c'),
  join_paths(zstd_rootdir, 'lib/common/pool.c'),
  join_paths(zstd_rootdir, 'lib/common/threading.c'),
  join_paths(zstd_rootdir, 'lib/common/zstd_common.c'),
  join_paths(zstd_rootdir, 'lib/common/error_private.c')]
poolTests = executable('poolTests',
  poolTests_sources,
  include_directories: test_includes,
  dependencies: [ testcommon_dep, thread_dep ],
  install: false)

checkTag_sources = [join_paths(zstd_rootdir, 'tests/checkTag.c')]
checkTag = executable('checkTag',
  checkTag_sources,
  dependencies: [ libzstd_dep ],
  install: false)

# =============================================================================
# Tests (Use "meson test --list" to list all tests)
# =============================================================================

if tests_supported_oses.contains(host_machine_os)
  valgrind_prog = find_program('valgrind', ['/usr/bin/valgrind'], required: true)
  valgrindTest_py = files('valgrindTest.py')
  test('valgrindTest',
    valgrindTest_py,
    args: [valgrind_prog.path(), zstd, datagen, fuzzer, fullbench],
    depends: [zstd, datagen, fuzzer, fullbench],
    timeout: 600) # Timeout should work on HDD drive
endif

if host_machine_os != os_windows
  playTests_sh = find_program(join_paths(zstd_rootdir, 'tests/playTests.sh'), required: true)
  test('test-zstd',
    playTests_sh,
    args: ZSTDRTTEST,
    env: ['ZSTD_BIN=' + zstd.full_path(), 'DATAGEN_BIN=./datagen'],
    depends: [datagen],
    workdir: meson.current_build_dir(),
    timeout: 2800) # Timeout should work on HDD drive
endif

test('test-fullbench-1',
  fullbench,
  args: ['-i1'],
  depends: [datagen],
  timeout: 60)
test('test-fullbench-2',
  fullbench,
  args: ['-i1', '-P0'],
  depends: [datagen],
  timeout: 60)

if use_zlib
  test('test-fuzzer',
    fuzzer,
    args: ['-v', FUZZERTEST] + FUZZER_FLAGS,
    timeout: 480)
endif

test('test-zstream-1',
  zstreamtest,
  args: ['-v', ZSTREAM_TESTTIME] + FUZZER_FLAGS,
  timeout: 240)
test('test-zstream-3',
  zstreamtest,
  args: ['--newapi', '-t1', ZSTREAM_TESTTIME] + FUZZER_FLAGS,
  timeout: 120)
test('test-longmatch', longmatch, timeout: 36)
test('test-invalidDictionaries', invalidDictionaries) # should be fast
if 0 < legacy_level and legacy_level <= 4
  test('test-legacy', legacy) # should be fast
endif
test('test-decodecorpus',
  decodecorpus,
  args: ['-t', DECODECORPUS_TESTTIME],
  timeout: 60)
test('test-poolTests', poolTests) # should be fast
