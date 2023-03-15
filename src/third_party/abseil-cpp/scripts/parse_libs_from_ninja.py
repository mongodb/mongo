#!/usr/bin/env python3

import os
import pathlib
import subprocess
import logging
import sys

# This is the list of libraries that are desired to be extracted out of the
# ninja file
target_libs = set([
    'absl_base',
    'absl_log_severity',
    'absl_malloc_internal',
    'absl_raw_logging_internal',
    'absl_spinlock_wait',
    'absl_throw_delegate',
    'absl_debugging_internal',
    'absl_demangle_internal',
    'absl_stacktrace',
    'absl_symbolize',
    'absl_int128',
    'absl_exponential_biased',
    'absl_random_distributions',
    'absl_random_internal_platform',
    'absl_random_internal_pool_urbg',
    'absl_random_internal_randen',
    'absl_random_internal_randen_hwaes',
    'absl_random_internal_randen_hwaes_impl',
    'absl_random_internal_randen_slow',
    'absl_random_internal_seed_material',
    'absl_random_seed_gen_exception',
    'absl_random_seed_sequences',
    'absl_status',
    'absl_cord',
    'absl_cord_internal',
    'absl_cordz_functions',
    'absl_cordz_handle',
    'absl_cordz_info',
    'absl_str_format_internal',
    'absl_strings',
    'absl_strings_internal',
    'absl_graphcycles_internal',
    'absl_synchronization',
    'absl_civil_time',
    'absl_time',
    'absl_time_zone',
    'absl_bad_optional_access',
    'absl_hashtablez_sampler',
    'absl_raw_hash_set',
    'absl_city',
    'absl_low_level_hash',
    'absl_hash',
    'absl_statusor',
    'absl_bad_variant_access',
])

# this is the path for cmake to use to generate abseil native ninja file.
cmake_bin_path = '/opt/mongodbtoolchain/v4/bin/cmake'

if sys.platform == 'win32' or sys.platform == 'darwin':
    raise Exception("This script is currently not supported on windows or macos.")

logging.basicConfig(
    filename=os.path.splitext(__file__)[0] + '.log', filemode='w',
    format='%(asctime)s,%(msecs)d %(name)s %(levelname)s %(message)s', datefmt='%H:%M:%S',
    level=logging.DEBUG)

original_target_libs = target_libs.copy()
logging.info(f'Original list: {original_target_libs}')

ninja_build_dir = pathlib.Path(__file__).parent.parent / 'dist' / 'scons_gen_build'
if not os.path.exists(ninja_build_dir):
    os.mkdir(ninja_build_dir)
    subprocess.run([cmake_bin_path, '-G', 'Ninja', '..'], cwd=ninja_build_dir, check=True)

with open(ninja_build_dir / 'build.ninja') as fninja:
    content = fninja.readlines()

with open(pathlib.Path(__file__).parent.parent / 'SConscript', 'w') as sconscript:
    sconscript.write("""\
# Generated from parse_lib_from_ninja.py   
Import("env")
env = env.Clone()
env.InjectThirdParty(libraries=['abseil-cpp'])
if env.ToolchainIs('msvc'):
    env.Append(
        CPPDEFINES=[
            'NOMINMAX',
        ],
        CCFLAGS=[],
    )

if env.GetOption('sanitize') and 'undefined' in env.GetOption('sanitize').split(','):
    # UBSAN causes the __muloti4 reference to be in the library. This is not defined in libgcc, so
    # we will just opt out of this check in this third party library. Related issues below:
    #
    # abseil issue showing the commit it was introduced
    # https://github.com/abseil/abseil-cpp/issues/841
    #
    # GCC bug saying the symbol is missing
    # https://gcc.gnu.org/bugzilla/show_bug.cgi?id=103034
    #
    # LLVM bug saying the symbol requires extra linkage
    # https://bugs.llvm.org/show_bug.cgi?id=16404
    env.Append(
        CCFLAGS=[
            '-fno-sanitize=signed-integer-overflow',
        ],
        LINKFLAGS=[
            '-fno-sanitize=signed-integer-overflow',
        ],
    )
""")

    # This will loop through the ninja file looking for the specified target libs.
    # A pass throught he ninja file may find one ore more libraries but must find
    # at least one or else an exception is raised.
    #
    # For each library found, the dependent libraries are added as libraries to find, and
    # so several passes the the ninja file may be required as new dependencies are found.
    written_libs = set()
    while written_libs != target_libs:

        cur_libs_num = len(written_libs)

        for line in content:

            # found an interesting line with potential, ninja build edges always start with
            # "build {targets}"
            if line.startswith('build absl'):
                found_target_lib = None

                # check if this intersting line is referring to one of the target libs
                for target_lib in target_libs:
                    if f'lib{target_lib}.a: CXX_STATIC_LIBRARY_LINKER' in line:
                        found_target_lib = target_lib

                # if the line does not contain our target lib or we already found this before
                # then this line is not interesting and continue on
                if not found_target_lib:
                    continue
                if found_target_lib in written_libs:
                    continue

                # else we have found a new library so lets parse out the source files and dependent
                # libraries. Ninja format use spaces as delimiters and $ as an escape. The loop below
                # while extract the spaces which should be exacped and put them in the tokens.
                raw_tokens = line.split(' ')
                tokens = []
                index = 0
                while index < len(raw_tokens):
                    token = raw_tokens[index]
                    while token.endswith('$'):
                        index += 1
                        token = token[:-1]
                        token += raw_tokens[index]
                    tokens.append(token)
                    index += 1

                # the dependent liraries will be listed after the explicit deps separator '||'
                try:
                    deps_token_index = tokens.index('||')
                except ValueError:
                    deps_token_index = len(tokens)

                # The source files should be after the "build" identifier, target name, and rule,
                # and before the dependencies for example:
                #
                # build absl/base/libabsl_strerror.a: CXX_STATIC_LIBRARY_LINKER__strerror_
                #
                # This would fail if the build edge listed multiple output targets, but this so far has not been
                # the case with abseil.
                raw_source_files = tokens[3:deps_token_index]
                source_files = []

                # because the source files are listed object file inputs to the static lib
                # we need strip the cmake output dir, and the object file extension
                for raw_source in raw_source_files:
                    path_elems = raw_source.split('/')
                    path_elems.remove('CMakeFiles')
                    path_elems.remove(found_target_lib.replace('absl_', '') + '.dir')

                    source_files.append(os.path.splitext(os.path.join('dist', *path_elems))[0])

                # now extract the library dependencies
                raw_libdeps = tokens[deps_token_index + 1:]
                libdeps = []

                for raw_libdep in raw_libdeps:
                    libdeps.append(f"{os.path.splitext(os.path.basename(raw_libdep))[0][3:]}")

                # When we have found a lib add it to our list of found libs and also
                # add any new dependencies we found to our target list.
                written_libs.add(found_target_lib)
                target_libs.update(libdeps)

                logging.info(f"Found library {found_target_lib}")
                logging.info(f"Libbraries left to find: {target_libs.difference(written_libs)}")

                sconscript.write(f"""\

{f'# {found_target_lib} added as a dependency of other abseil libraries' 
if found_target_lib not in original_target_libs 
else f'# {found_target_lib} is an explicit dependency to the server build'}
env.Library(
    target='{found_target_lib}',
    source=[
{os.linesep.join([f"        '{source}'," for source in source_files])}
    ],
    LIBDEPS=[
{os.linesep.join([f"        '{libdep}'," for libdep in sorted(libdeps)])}
    ],
)
""")

        if len(written_libs) == cur_libs_num:
            raise Exception(
                f"Did not find any more requested libs {target_libs.difference(written_libs)}, " +
                "the library must exist in the abseil build, check the build.ninja file " +
                "and the parse_lib_from_ninja.log file.")
