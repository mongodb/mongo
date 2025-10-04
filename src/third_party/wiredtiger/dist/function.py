#!/usr/bin/env python3

# Check the style of WiredTiger C code.
import os, re, sys
from dist import all_c_files, all_cpp_files, all_h_files, compare_srcfile, source_files
from common_functions import filter_if_fast

def check_function_comment(function_name, function_comment):
    # Unit test functions don't have to have a comment.
    if function_name.startswith('__ut_'):
        return True

    # No comment at all.
    if not function_comment:
        return False

    # The first line is the function name
    #    /*
    #     * func_name --
    if function_comment.startswith(f'/*\n * {function_name} --\n'):
        return True

    # Unformatted comment containing !!!
    #    /*
    #     * !!!
    #     * func_name --
    if function_comment.find('!!!') != -1 and \
            function_comment.find(f'\n * {function_name} --\n') != -1:
        return True

    return False

# Complain if a function comment is missing.
def missing_comment():
    for f in filter_if_fast(all_c_files(), prefix="../"):
        skip_re = re.compile(r'DO NOT EDIT: automatically built')
        func_re = re.compile(
            r'(/\*(?:[^\*]|\*[^/])*\*/)?\n\w[\w \*]+\n(\w+)', re.DOTALL)
        s = open(f, 'r').read()
        if skip_re.search(s):
            continue
        for m in func_re.finditer(s):
            function_name, function_comment = m.group(2), m.group(1)
            if not check_function_comment(function_name, function_comment):
                line_num = s[:m.start(2)].count('\n')
                print(f"{f}:{line_num}: malformed comment for {function_name}")

# Sort helper function, discard * operators so a pointer doesn't necessarily
# sort before non-pointers, ignore const/static/volatile keywords.
def function_args_alpha(text):
        s = text.strip()
        s = re.sub("[*]","", s)
        s = s.split()
        def merge_specifier(words, specifier):
            if len(words) > 2 and words[0] == specifier:
                words[1] += specifier
                words = words[1:]
            return words
        s = merge_specifier(s, 'const')
        s = merge_specifier(s, 'static')
        s = merge_specifier(s, 'volatile')
        s = ' '.join(s)
        return s

# List of illegal types.
illegal_types = [
    'u_int16_t',
    'u_int32_t',
    'u_int64_t',
    'u_int8_t',
    'u_quad',
    'uint '
]

# List of legal types in sort order.
types = [
    'struct',
    'union',
    'enum',
    'DIR',
    'FILE',
    'TEST_',
    'WT_',
    'wt_',
    'DWORD',
    'double',
    'float',
    'intmax_t',
    'intptr_t',
    'clock_t',
    'pid_t',
    'pthread_t',
    'size_t',
    'ssize_t',
    'time_t',
    'uintmax_t',
    'uintptr_t',
    'u_long',
    'long',
    'uint64_t',
    'int64_t',
    'uint32_t',
    'int32_t',
    'uint16_t',
    'int16_t',
    'uint8_t',
    'int8_t',
    'u_int',
    'int',
    'u_char',
    'char',
    'bool',
    'va_list',
    'void '
]

# Return the sort order of a variable declaration, or no-match.
#       This order isn't defensible: it's roughly how WiredTiger looked when we
# settled on a style, and it's roughly what the KNF/BSD styles look like.
def function_args(name, line):
    line = line.strip()
    line = re.sub("^const ", "", line)
    line = re.sub("^static ", "", line)
    line = re.sub("^volatile ", "", line)

    # Let WT_ASSERT, WT_UNUSED and WT_RET terminate the parse. They often appear
    # at the beginning of the function and looks like a WT_XXX variable
    # declaration.
    if re.search('^WT_ASSERT', line):
        return False,0
    if re.search('^WT_UNUSED', line):
        return False,0
    if re.search('^WT_RET', line):
        return False,0

    # If one or more of the variables is being initialised, then dependencies
    # may exist that prevent alphabetical ordering.
    # For example, the following lines cannot be sorted alphabetically:
    #    WT_BTREE *btree = S2BT(session);
    #    WT_BM *bm = btree->bm;
    # So, in the presence of initialisation, terminate the parse.
    if re.search('=', line):
        return False,0

    # Let lines not terminated with a semicolon terminate the parse, it means
    # there's some kind of interesting line split we probably can't handle.
    if not re.search(';$', line):
        return False,0

    # Check for illegal types.
    for m in illegal_types:
        if re.search('^' + m + r"\s*[\w(*]", line):
            print(name + ": illegal type: " + line.strip(), file=sys.stderr)
            sys.exit(1)

    # Check for matching types.
    for n,m in enumerate(types, 0):
        # Don't list '{' as a legal character in a declaration, that's what
        # prevents us from sorting inline union/struct declarations.
        if re.search('^' + m + r"\s*[\w(*]", line):
            return True,n
    return False,0

# Put function arguments in correct sort order.
def function_declaration():
    tmp_file = '__tmp_function' + str(os.getpid())
    for name in filter_if_fast(all_c_files(), prefix="../"):
        skip_re = re.compile(r'DO NOT EDIT: automatically built')
        s = open(name, 'r').read()
        if skip_re.search(s):
            continue

        # Read through the file, and for each function, do a style pass over
        # the local declarations. Quit tracking declarations as soon as we
        # find anything we don't understand, leaving it untouched.
        with open(name, 'r') as f:
            tfile = open(tmp_file, 'w')
            tracking = False
            for line in f:
                if not tracking:
                    tfile.write(line)
                    if re.search('^{$', line):
                        list = [[] for i in range(len(types))]
                        static_list = [[] for i in range(len(types))]
                        tracking = True;
                    continue

                found,n = function_args(name, line)
                if found:
                    # List statics first.
                    if re.search(r"^\s+static", line):
                        static_list[n].append(line)
                        continue

                    list[n].append(line)
                else:
                    # Sort the resulting lines (we don't yet sort declarations
                    # within a single line). It's two passes, first to catch
                    # the statics, then to catch everything else.
                    for arg in filter(None, static_list):
                        for p in sorted(arg, key=function_args_alpha):
                            tfile.write(p)
                    for arg in filter(None, list):
                        for p in sorted(arg, key=function_args_alpha):
                            tfile.write(p)
                    tfile.write(line)
                    tracking = False
                    continue

            tfile.close()
            compare_srcfile(tmp_file, name)

# A function definition.
class FunctionDefinition:
    def __init__(self, name, module, source):
        self.name = name
        self.module = module
        self.source = source
        self.used_outside_of_file = False
        self.used_outside_of_module = False
        self.visibility_global = name.startswith('__wt_')
        self.visibility_module = name.startswith('__wti_')

# Check if the file is auto-generated based on its contents (either a string or a list of lines).
def is_auto_generated(file_contents):
    skip_re = re.compile(r'DO NOT EDIT: automatically built')
    if type(file_contents) is not list:
        file_contents = [file_contents]
    for s in file_contents:
        if skip_re.search(s):
            return True
    return False

# Check whether functions are used in their expected scopes:
#   * "__wti" functions are only used or called by other files within that directory.
#   * "__wti" functions are used in at least two files (otherwise they could be made static).
#   * "__wt" functions are used outside of their directory (otherwise they could be made "__wti").
#
# It uses the same exceptions list as "s_funcs" (i.e., s_funcs.list), which ensures that all extern
# functions are used.
def function_scoping():
    func_def_re = re.compile(r'\n\w[\w \*]+\n(\w+)\(', re.DOTALL)
    func_use_re = re.compile(r'[^\n](\w+)', re.DOTALL)
    failed = False

    # Infer the module name from the file path.
    def infer_module(path):
        parts = path.split('/')
        if len(parts) < 3 or parts[0] != '..':
            print(f'{path}: Unexpected path')
            sys.exit(1)

        # If the path is in src, the module is the subdirectory under src.
        # If the path is test/unittest, the module is unittest.
        # Otherwise it is the top-level directory, such as "test."
        if parts[1] == 'src':
            module = parts[2]
        elif parts[1] == "test" and parts[2] == "catch2":
            # Treat test/unittests special since they are allowed to call __wti_ functions.
            module = parts[2]
        else:
            module = parts[1]

        # Porting layer contains duplicate function names across platforms. So treat as a single
        # module to avoid false duplicates below.
        os_ports = ['os_posix', 'os_win', 'os_darwin', 'os_linux']
        if module in os_ports:
            return '/'.join(os_ports)
        return module

    # Find all "__wt" and "__wti" function definitions.
    fn_defs = dict()
    for source_file in source_files():
        if not source_file.startswith('../src/'):
            continue
        module = infer_module(source_file)

        # This script assumes any file containing a DO NOT EDIT line should be 
        # skipped for checking, however this is not applicable to header files
        # generated by prototypes.py. Only the section inside the DO NO EDIT 
        # should be skipped. Ignore these lines when processing files.
        file_contents = open(source_file, 'r').readlines()
        is_proto = "/* DO NOT EDIT: automatically built by prototypes.py: BEGIN */\n" \
            in file_contents
        if is_proto:
            start_def = file_contents.index(
                "/* DO NOT EDIT: automatically built by prototypes.py: BEGIN */\n")
            end_def = file_contents.index(
                "/* DO NOT EDIT: automatically built by prototypes.py: END */\n")
            file_contents = file_contents[:start_def] + file_contents[end_def + 1:]
        file_contents = "".join(file_contents)

        # Skip auto-generated files.
        if is_auto_generated(file_contents):
            continue

        # Now actually find all definitions.
        for m in func_def_re.finditer(file_contents):
            fn_name = m.group(1)
            if not fn_name.startswith('__wt_') and not fn_name.startswith('__wti_'):
                continue
            if fn_name in fn_defs and fn_defs[fn_name].module != module:
                print(f'{source_file}: {fn_name} is already defined in ' +
                      f'module "{fn_defs[fn_name].module}"')
                sys.exit(1)
            fn_defs[fn_name] = FunctionDefinition(fn_name, module, source_file)

    # Find and check all "__wt" and "__wti" function uses.
    files = []
    files.extend(all_c_files())
    files.extend(all_cpp_files())
    files.extend(all_h_files())
    for source_file in files:
        if source_file.startswith('../src/include/extern'):
            continue
        module = infer_module(source_file)

        # Skip auto-generated files.
        file_lines = open(source_file, 'r').readlines()
        if is_auto_generated(file_lines):
            continue

        # Find all uses. Check the use of "__wti" functions.
        in_block_comment = False
        line_no = 0
        for line in file_lines:
            line_no += 1

            # Skip block and line comments.
            s = line.strip()
            if s.startswith('//') or (s.startswith('/*') and s.endswith('*/')):
                continue
            if s == '/*':
                in_block_comment = True
                continue
            if in_block_comment:
                if s.endswith('*/'):
                    in_block_comment = False
                continue

            # Find all function uses in each line.
            for m in func_use_re.finditer(line):
                fn_name = m.group(1)
                if not fn_name.startswith('__wt_') and not fn_name.startswith('__wti_'):
                    continue
                # Undefined functions are either macros or test functions; just skip them.
                if fn_name not in fn_defs:
                    continue

                # Remember whether the function was used outside of its module and of its file.
                fn = fn_defs[fn_name]
                if module != fn.module:
                    fn.used_outside_of_module = True
                if source_file != fn.source:
                    fn.used_outside_of_file = True

                # Check whether a "__wti" function is used outside of its module.
                # Exception: Unittest files are allowed to call __wti_ functions.
                if fn.visibility_module and module != fn.module and module != "catch2":
                    print(f'{source_file}:{line_no}: {fn_name} is used outside of its module ' +
                          f'"{fn.module}"')
                    failed = True

    # Load the list of functions whose scope is not enforced.
    exceptions = set(l.strip() for l in open('s_funcs.list', 'r').readlines())

    # Check whether any "__wt" functions are used only within the same module (and could be thus
    # turned into "__wti" functions). Functions in "include" are implicitly used in more than one
    # module.
    for fn_name, d in fn_defs.items():
        if not d.visibility_global:
            continue
        if fn_name in exceptions:
            continue
        if d.module == 'include':
            continue
        if not d.used_outside_of_module:
            print(f'{d.source}: {fn_name} is NOT used outside of its module "{d.module}"')
            failed = True

    # Check whether any "__wti" functions are used only within the same file. Skip functions in
    # "include", because they are implicitly used in more than one file.
    for fn_name, d in fn_defs.items():
        if not d.visibility_module:
            continue
        if fn_name in exceptions:
            continue
        if d.module == 'include':
            continue
        if not d.used_outside_of_file:
            print(f'{d.source}: {fn_name} is NOT used outside of its source file')
            failed = True

    if failed:
        sys.exit(1)

# Report missing function comments.
missing_comment()

# Update function argument declarations.
function_declaration()

# Report functions that are used outside of their intended scope.
function_scoping()
