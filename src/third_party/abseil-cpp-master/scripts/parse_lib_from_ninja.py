import sys
import os

with open(sys.argv[1]) as fninja:
    content = fninja.readlines()

target_libs_file = sys.argv[2]

with open(target_libs_file) as ftargets:
    target_libs = [ft.strip() for ft in ftargets.readlines()]
for line in content:
    if line.startswith('build absl'):
        found_target_lib = None
        for target_lib in target_libs:

            if f'lib{target_lib}.a: CXX_STATIC_LIBRARY_LINKER' in line:
                found_target_lib = target_lib
        if not found_target_lib:
            continue

        tokens = line.split(' ')
        try:
            deps_token_index = tokens.index('||')
        except ValueError:
            deps_token_index = len(tokens)

        raw_source_files = tokens[3:deps_token_index]
        source_files = []
        for raw_source in raw_source_files:
            path_elems = raw_source.split('/')
            path_elems.remove('CMakeFiles')
            path_elems.remove(found_target_lib.replace('absl_','') + '.dir')

            source_files.append(os.path.splitext(os.path.join('abseil-cpp', *path_elems))[0])
        raw_libdeps = tokens[deps_token_index+1:]
        libdeps = []
        for raw_libdep in raw_libdeps:
            libdeps.append(f"{os.path.splitext(os.path.basename(raw_libdep))[0][3:]}")

        scons_out = (f"""\
env.Library(
    target='{found_target_lib}',
    source=[
{os.linesep.join([f"        '{source}'," for source in source_files])}
    ],
    LIBDEPS=[
{os.linesep.join([f"        '{libdep}'," for libdep in sorted(libdeps)])}
    ]
)
"""
        )
        print(scons_out)
