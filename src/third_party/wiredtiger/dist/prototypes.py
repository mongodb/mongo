#!/usr/bin/env python3

# This script parses the WiredTiger code base to find all function definitions and automatically 
# generates their declarations. Declarations are added to src/include/extern.h by default, or to 
# module-specific header files if the module is added to the SELF_CONTAINED_MODULES list

# Attention! This script makes the following assumptions about the WiredTiger code directory:
# - All module-related files should reside in a single src/module/ folder as a result of adding 
#   a module to SELF_CONTAINED_MODULES
# - Header files are named using the format {folder_name/module_name}.h or 
#   {folder_name/module_name}_private.h.
# - Header files that are modified by this script have also been automatically created by this 
#   script as a result of adding a module to the SELF_CONTAINED_MODULES list.

# Generate WiredTiger function prototypes.
import fnmatch, re, os, sys
from dist import compare_srcfile, format_srcfile, source_files
from common_functions import filter_if_fast

from collections import defaultdict

# This is the list of modules that have their respective headers located in the src/module/ folder 
# instead of in src/include/. As a result all relevant code is contained to a single folder. 
# Adding modules to this list will automatically generate those headers, and we expect this list to 
# grow as we modularise the code base.
SELF_CONTAINED_MODULES = ["checkpoint", "evict", "log", "reconcile", "live_restore"]

DO_NOT_EDIT_BEGIN = "/* DO NOT EDIT: automatically built by prototypes.py: BEGIN */\n"
DO_NOT_EDIT_END = "/* DO NOT EDIT: automatically built by prototypes.py: END */\n"

COPYRIGHT = """\
/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 * All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

"""

files = [f for f in source_files()]
if not [f for f in filter_if_fast(files + ["../dist/prototypes.py"], prefix="../")]:
    sys.exit(0)

def clean_function_name(filename, fn):
    ret = fn.strip()

    # Ignore statics in XXX.c files.
    if fnmatch.fnmatch(filename, "*.c") and 'static' in ret:
        return None

    # Join the first two lines, type and function name.
    ret = ret.replace("\n", " ", 1)

    # If there's no CPP syntax, join everything.
    if not '#endif' in ret:
        ret = " ".join(ret.split())

    # If it's not an inline function, prefix with "extern".
    if 'inline' not in ret and 'WT_INLINE' not in ret:
        ret = 'extern ' + ret

    # Switch to the include file version of any gcc attributes.
    ret = ret.replace("WT_GCC_FUNC_ATTRIBUTE", "WT_GCC_FUNC_DECL_ATTRIBUTE")

    # Everything but void requires using any return value.
    if not re.match(r'(static inline|static WT_INLINE|extern) void', ret):
        ret = ret + " WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result))"

    # If a line ends in #endif, appending a semicolon results in an illegal
    # expression, force an appended newline.
    if re.match(r'#endif$', ret):
        ret = ret + '\n'

    return ret + ';\n'

# Find function prototypes in a file matching a given regex. Cleans the
# function names to the point being immediately usable.
def extract_prototypes(filename, regexp):
    ret = []
    s = open(filename, 'r').read()
    for p in re.findall(regexp, s):
        clean = clean_function_name(filename, p)
        if clean is not None:
            ret.append(clean)

    return ret

# Build function prototypes from a list of files.
def fn_prototypes(public_fns, private_fns, tests, name):
    for sig in extract_prototypes(name, r'\n[A-Za-z_].*\n__wt_[^{]*'):
        public_fns.append(sig)

    for sig in extract_prototypes(name, r'\n[A-Za-z_].*\n__wti_[^{]*'):
        private_fns.append(sig)

    for sig in extract_prototypes(name, r'\n[A-Za-z_].*\n__ut_[^{]*'):
        tests.append(sig)

# Create a new header file with function declarations.
# As this is a new file also generate boilerplate like #pragma once
def create_new_header(tmp_file, fns, tests):
    tfile = open(tmp_file, 'w')
    tfile.write(COPYRIGHT)
    tfile.write("#pragma once\n\n")

    tfile.write(f"{DO_NOT_EDIT_BEGIN}\n")

    for e in sorted(list(set(fns))):
        tfile.write(e)

    tfile.write('\n#ifdef HAVE_UNITTEST\n')
    for e in sorted(list(set(tests))):
        tfile.write(e)

    tfile.write('\n#endif\n')
    tfile.write(f"\n\n{DO_NOT_EDIT_END}")
    tfile.close()

# Update an existing header file with function declarations.
# As this file already exists only modify content between the DO NOT EDIT flags.
def update_existing_header(tmp_file, fns, tests, f):
    with open(f, 'r') as file:
        lines = file.readlines()

    if DO_NOT_EDIT_BEGIN not in lines or DO_NOT_EDIT_END not in lines:
        print(f"Error: File {f} is missing the lines \n{DO_NOT_EDIT_BEGIN} and \
              \n{DO_NOT_EDIT_END} required by prototypes.py. Both lines must \
              be followed by a newline")
        sys.exit(1)

    start_line = lines.index(DO_NOT_EDIT_BEGIN)
    end_line = lines.index(DO_NOT_EDIT_END)


    # Content before the starting DO NOT EDIT is kept unchanged.
    new_lines = lines[:start_line + 1]
    new_lines.append("\n") # maintain the new line after START

    # Replace the function prototypes between the DO NOT EDIT FLAGS
    for e in sorted(list(set(fns))):
        new_lines.append(e)

    new_lines.append('\n#ifdef HAVE_UNITTEST\n')
    for e in sorted(list(set(tests))):
        new_lines.append(e)

    new_lines.append('\n#endif\n')

    # Finally, retain all content after the end DO NOT EDIT flag
    new_lines.append("\n") # maintain the new line before END
    new_lines.extend(lines[end_line:])

    with open(tmp_file, 'w') as file:
        file.writelines(new_lines)

# Write results and compare to the current file.
# Unit-testing functions are exposed separately in their own section to
# allow them to be ifdef'd out.
def output(fns, tests, f):
    tmp_file = '__tmp_prototypes' + str(os.getpid())

    if not os.path.isfile(f):
        create_new_header(tmp_file, fns, tests)
    else:
        update_existing_header(tmp_file, fns, tests, f)

    format_srcfile(tmp_file)
    compare_srcfile(tmp_file, f)

# Build a mapping from a module to its public functions, private functions, 
# and HAVE_UNITTEST functions.
def build_module_functions_dicts():
    public_fns_dict = defaultdict(list)
    private_fns_dict = defaultdict(list)
    tests_dict = defaultdict(list)
    
    for name in source_files():
        if not fnmatch.fnmatch(name, '*.c') + fnmatch.fnmatch(name, '*_inline.h'):
            continue
        if fnmatch.fnmatch(name, '*/checksum/arm64/*'):
            continue
        if fnmatch.fnmatch(name, '*/checksum/loongarch64/*'):
            continue
        if fnmatch.fnmatch(name, '*/checksum/power8/*'):
            continue
        if fnmatch.fnmatch(name, '*/checksum/riscv64/*'):
            continue
        if fnmatch.fnmatch(name, '*/checksum/zseries/*'):
            continue
        if re.match(r'^.*/os_(?:posix|win|linux|darwin)/.*', name):
            # Handled separately in prototypes_os().
            continue
        if fnmatch.fnmatch(name, '*/ext/*'):
            continue

        if fnmatch.fnmatch(name, '../src/*'):
            module_name = name.split("/")[2]
            if module_name not in SELF_CONTAINED_MODULES:
                # Non-self-contained modules put all their function prototypes in 
                # src/include/extern.h This is indicated by belonging to the include folder.
                fn_prototypes(public_fns_dict["include"], private_fns_dict["include"], 
                    tests_dict["include"], name)
            else:
                fn_prototypes(public_fns_dict[module_name], private_fns_dict[module_name], 
                    tests_dict[module_name], name)
        else:
            print(f"Unexpected filepath {name}")
            sys.exit(1)

    return (public_fns_dict, private_fns_dict, tests_dict)

# Given a list of dicts that map a module to their public, private, and HAVE_UNITTEST functions, 
# write their header files with function declarations.
def write_header_files(public_fns_dict, private_fns_dict, tests_dict):
    # Trust that the public functions dict lists all modules. 
    # If a module doesn't have a public function then it can't be accessed and is dead code.
    modules = public_fns_dict.keys()
    for mod in modules:
        if mod == "include":
            # Functions defined in the include folder belong in extern.h
            output(public_fns_dict[mod] + private_fns_dict[mod], tests_dict[mod], 
                f"../src/include/extern.h")
        else:
            output(public_fns_dict[mod], tests_dict[mod], f"../src/{mod}/{mod}.h")
            if len(private_fns_dict[mod]) > 0:
                # The second argument (tests_dict) is empty. These test functions are defined to
                # expose module internals outside the module, so it doens't make sense for them 
                # to be private.
                output(private_fns_dict[mod], {}, f"../src/{mod}/{mod}_private.h")

def prototypes_os():
    """
    The operating system abstraction layer duplicates function names. So each 
    os gets its own extern header file.
    """
    ports = 'posix win linux darwin'.split()
    fns = {k:[] for k in ports}
    tests = {k:[] for k in ports}
    for name in source_files():
        if m := re.match(r'^.*/os_(posix|win|linux|darwin)/.*', name):
            port = m.group(1)
            assert port in ports
            # The operating system folders have special handling and write all functions 
            # into a single extern_*.h file. Save all functions into the same fns list.
            fn_prototypes(fns[port], fns[port], tests[port], name)

    for p in ports:
        output(fns[p], tests[p], f"../src/include/extern_{p}.h")

# Newly generated public headers need to be included from wt_internal.h.
# Add a warning reminding developers to do so.
def check_wt_internal_includes():
    with open("../src/include/wt_internal.h", 'r') as file:
        lines = file.readlines()

        for mod in SELF_CONTAINED_MODULES:
            include_line = f"#include \"../{mod}/{mod}.h\""
            if f"{include_line}\n" not in lines:
                print(f"The line '{include_line}' is missing from wt_internal.h. "
                      "Please make sure to include it.")
                exit(1)

(pub_fns_dict, private_fns_dict, tests_dict) = build_module_functions_dicts()
write_header_files(pub_fns_dict, private_fns_dict, tests_dict)
prototypes_os()
check_wt_internal_includes()
