"""Utility functions for udb."""

import os
import re
from typing import Optional
import gdb

# Pattern to match output of 'info files'
PATTERN_ELF_SECTIONS = re.compile(
    r'(?P<begin>[0x0-9a-fA-F]+)\s-\s(?P<end>[0x0-9a-fA-F]+)\s\bis\b\s(?P<section>\.[a-z]+$)')


def parse_sections():
    """Find addresses for .text, .data, and .bss sections."""
    file_info = gdb.execute('info files', to_string=True)
    section_map = {}
    for line in file_info.splitlines():
        line = line.strip()
        match = PATTERN_ELF_SECTIONS.match(line)
        if match is None:
            continue

        section = match.group('section')
        if section not in ('.text', '.data', '.bss'):
            continue
        begin = match.group('begin')
        section_map[section] = begin

    return section_map


def load_sym_file_at_addrs(dbg_file, smap):
    """Invoke add-symbol-file with addresses."""
    cmd = 'add-symbol-file {} {} -s .data {} -s .bss {}'.format(dbg_file, smap['.text'],
                                                                smap['.data'], smap['.bss'])
    gdb.execute(cmd, to_string=True)


class LoadDebugFile(gdb.Command):
    """Loads the debug symbol file with the correct address for .text, .data and .bss sections."""

    def __init__(self):
        """GDB Command API init."""
        super(LoadDebugFile, self).__init__('load-debug-symbols', gdb.COMPLETE_EXPRESSION)

    def invoke(self, args, from_tty):
        """GDB Command API invoke."""
        arglist = args.split()
        if len(arglist) != 1:
            print('Usage: load-debug-symbols <file_path>')
            return

        dbg_file = arglist[0]
        if not os.path.exists(dbg_file):
            print('{} is not a valid file path'.format(dbg_file))
            return

        try:
            section_map = parse_sections()
            load_sym_file_at_addrs(dbg_file, section_map)
        except Exception as err:  # pylint: disable=broad-except
            print(err)


LoadDebugFile()

PATTERN_ELF_SOLIB_SECTIONS = re.compile(
    r'(?P<begin>[0x0-9a-fA-F]+)\s-\s(?P<end>[0x0-9a-fA-F]+)\s\bis\b\s(?P<section>\.[a-z]+)\s\bin\b\s(?P<file>.*$)'
)


def parse_solib_sections():
    """Find addresses for .text, .data, and .bss sections."""
    file_info = gdb.execute('info files', to_string=True)
    section_map = {}
    for line in file_info.splitlines():
        line = line.strip()
        match = PATTERN_ELF_SOLIB_SECTIONS.match(line)
        if match is None:
            continue

        section = match.group('section')
        if section not in ('.text', '.data', '.bss'):
            continue
        begin = match.group('begin')
        # TODO duplicate fnames?
        fname = os.path.basename(match.group('file'))

        if fname.startswith("system-supplied DSO") or match.group('file').startswith(
                "/lib") or match.group('file').startswith("/usr/lib"):
            continue
        fname = f"{fname}.debug"
        section_map.setdefault(fname, {})
        section_map[fname][section] = begin

    return section_map


def is_probably_dwarf_file(fname):
    """Check if it's a file, and ends in .debug."""
    return os.path.isfile(fname) and fname.endswith(".debug")


def find_dwarf_files(path):
    """Given a directory, collect a list of files in it that pass the is_probably_dwarf_file test."""
    out = []
    for fname in os.listdir(path):
        full_path = os.path.join(path, fname)
        if is_probably_dwarf_file(full_path):
            out.append(full_path)
    return out


SOLIB_SEARCH_PATH_PREFIX = "The search path for loading non-absolute shared library symbol files is "


def extend_solib_search_path(new_path: str):
    """Extend solib-search-path."""
    solib_search_path = gdb.execute("show solib-search-path", to_string=True)
    # remove the prefix and suffix (which is a period and space) from the
    # search path
    solib_search_path = solib_search_path[len(SOLIB_SEARCH_PATH_PREFIX):-2]
    solib_search_path = f"{new_path}:{solib_search_path}"
    if solib_search_path.endswith(":"):
        solib_search_path = solib_search_path[:-1]

    gdb.execute(f"set solib-search-path {solib_search_path}", to_string=True)


DEBUG_FILE_DIRECTORY_PREFIX = 'The directory where separate debug symbols are searched for is "'


def extend_debug_file_directory(new_path: str):
    """Extend debug-file-directory."""
    debug_file_directory = gdb.execute("show debug-file-directory", to_string=True)
    # remove the prefix and suffix (which is a period and space) from the
    # search path
    debug_file_directory = debug_file_directory[len(DEBUG_FILE_DIRECTORY_PREFIX):-3]
    debug_file_directory = f"{new_path}:{debug_file_directory}"
    if debug_file_directory.endswith(":"):
        debug_file_directory = debug_file_directory[:-1]

    gdb.execute(f"set debug-file-directory {debug_file_directory}", to_string=True)


class LoadDistTest(gdb.Command):
    """Load all symbol files in a dist-test directory.

    Command assumes provided directory has a bin and lib subdir, and will
    add-symbol-file all .debug files in those directories.
    """

    def __init__(self):
        """GDB Command API init."""
        super(LoadDistTest, self).__init__('load-dist-test', gdb.COMPLETE_EXPRESSION)

        try:
            # test if we're running udb
            gdb.execute("help uinfo", to_string=True)
            self._is_udb = True
        except gdb.error:
            self._is_udb = False

    @staticmethod
    def binary_name():
        """Fetch the name of the binary gdb is attached to."""
        main_binary_name = gdb.objfiles()[0].filename
        main_binary_name = os.path.splitext(os.path.basename(main_binary_name))[0]
        if main_binary_name.endswith('mongod'):
            return "mongod"
        if main_binary_name.endswith('mongo'):
            return "mongo"
        if main_binary_name.endswith('mongos'):
            return "mongos"

        return None

    def invoke(self, args, from_tty):
        """GDB Command API invoke."""
        arglist = args.split()
        if not arglist:
            arglist = [os.path.abspath("./dist-test")]
            print(f"No path provided, assuming '{arglist[0]}'")

        if len(arglist) != 1:
            print('Usage: load-dist-test <path/to/dist-test>')
            return

        dist_test = arglist[0]
        if not os.path.isdir(dist_test):
            print(f"'{dist_test}' does not exist, or is not a directory")
            return

        if self._is_udb:
            # if this is an instance of udb, save a bookmark for the current
            # location, and jump to the recording's end
            # to make sure the shared libraries have been dlopen'd. This
            # ensures that when we try to parse the solib sections, we know
            # we have the .text, .bss, .data section addresses available
            gdb.execute("ubookmark ____dist_test", to_string=True)
            gdb.execute("ugo end", to_string=True)

        dwarf_files = []

        bin_dir = os.path.join(dist_test, "bin")
        if bin_dir:
            dwarf_files.extend(find_dwarf_files(bin_dir))
            extend_debug_file_directory(bin_dir)

        lib_dir = os.path.join(dist_test, "lib")
        if lib_dir:
            dwarf_files.extend(find_dwarf_files(lib_dir))
            extend_solib_search_path(lib_dir)

        if not dwarf_files:
            return

        yell_at_user_main_bin = False
        try:
            print("Loading symbols. This will take a while..")
            main_bin = LoadDistTest.binary_name()
            if main_bin:
                # if we know the name of this executable, load its symbol file
                main_bin_sections = parse_sections()
                dbg_file = os.path.join(dist_test, "bin", f"{main_bin}.debug")
                load_sym_file_at_addrs(dbg_file, main_bin_sections)

            else:
                # if we print here, the message will get lost in the noise, so
                # we delay that until the end of this function
                yell_at_user_main_bin = True

            section_map = parse_solib_sections()
            for idx, dwarf_file in enumerate(dwarf_files):
                base_name = os.path.basename(dwarf_file)
                if base_name not in section_map:
                    continue

                load_sym_file_at_addrs(dwarf_file, section_map[base_name])
                if (idx + 1) % 50 == 0 or len(dwarf_files) == idx + 1:
                    print(f"{idx+1}/{len(dwarf_files)} symbol files loaded")

        except Exception as err:  # pylint: disable=broad-except
            print(err)

        if self._is_udb:
            # if this is an instance of udb, jump to the bookmark we made
            # earlier to be nice to the user
            gdb.execute("ugo bookmark ____dist_test", to_string=True)

        if yell_at_user_main_bin:
            print(
                f"Failed to automagically locate debug symbols for main binary. Try loading them manually, 'load-debug-symbols {dist_test}/bin/[your_binary_symbols.debug]'"
            )
            print("^^^^^^ HEY LISTEN ^^^^^^")


LoadDistTest()
