#!/usr/bin/env python3
"""
TOOL FUNCTIONAL DESCRIPTION.

Currently the tool works by running IWYU on a subset of compile_commands.json
(the ones we care about like checked in mongo source) and testing each change
in a copy of the original source/header tree so that other compiles are not
affected until it passes a normal compile itself. Due to header dependencies
we must recompile the source files to catch issue IWYU may have introduced
with some dependent header change. Header dependencies do not form a DAG so
we can not process sources in a deterministic fashion. The tool will loop
through all the compilations until all dependents in a compilation are
determined unchanged from the last time the compilation was performed.

The general workflow used here is to run the tool till there no changes
(several hours on rhel-xxlarge) and fix the errors either in the tool config
or as a manual human change in the code.

TOOL TECHNICAL DESCRIPTION:

Regarding the code layout, the main function setups a thread pool executor
and processes each source from the compile_commands. From there it runs a
thread function and within that 5 parts (each there own function) for
each source file:

1. Skip if deps are unchanged
2. Get the headers deps via -MMD
3. Run IWYU
4. Apply Fixes
5. test compile, record new header deps if passed

The tool uses mtime and MD5 hashing to know if any header dep has changed.

"""

import argparse
import atexit
import concurrent.futures
import enum
import hashlib
import json
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import traceback
from dataclasses import asdict, dataclass
from typing import Any, Callable, Dict, List, Optional, Tuple, Union

import yaml
from colorama import Fore
from colorama import init as colorama_init
from tqdm import tqdm

colorama_init()

parser = argparse.ArgumentParser(description="Run include what you use and test output")

parser.add_argument(
    "--compile-commands",
    metavar="FILE",
    type=str,
    default="compile_commands.json",
    help="Path to the compile commands file to use.",
)
parser.add_argument(
    "--check",
    action="store_true",
    help="Enables check mode, which does not apply fixes and only runs to see if any files produce IWYU changes. Exit 0 if no new changes detected.",
)
parser.add_argument(
    "--config-file",
    metavar="FILE",
    type=str,
    default="",
    help="Enables check mode, which does not apply fixes and only runs to see if any files produce IWYU changes. Exit 0 if no new changes detected.",
)
parser.add_argument(
    "--iwyu-data",
    metavar="FILE",
    type=str,
    default="iwyu.dat",
    help="Location of data used by IWYU, contains hash and status info about all files.",
)
parser.add_argument(
    "--keep-going",
    action="store_true",
    help="Do not stop on errors, instead resubmit the job to try again later (after things may have been fixed elsewhere)",
)
parser.add_argument(
    "--cycle-debugging",
    action="store_true",
    help="Once a cycle has been detected, each directory tree for each step in the cycle will be saved to a .cycle directory.",
)
parser.add_argument(
    "--verbose", action="store_true", help="Prints more info about what is taking place."
)
parser.add_argument(
    "--mongo-toolchain-bin-dir",
    type=str,
    help="Which toolchain bin directory to use for this analysis.",
    default="/opt/mongodbtoolchain/v4/bin",
)
parser.add_argument(
    "--start-ratio",
    type=float,
    help="decimal value between 0 and 1 which indicates what starting ratio index of the total compile commands to run over, can not be greater than the --end-ratio.",
    default=0.0,
)
parser.add_argument(
    "--end-ratio",
    type=float,
    help="decimal value between 0 and 1 which indicates what ending ratio index of the total compile commands to run over, can not be less than the --start-ratio.",
    default=1.0,
)
command_line_args = parser.parse_args()

# the current state of all files, contain the cmd_entry, hashes, successes
IWYU_ANALYSIS_STATE: Dict[str, Any] = {}

# the current state cycles being tracked
IWYU_CYCLE_STATE: Dict[str, Any] = {}

hash_lookup_locks: Dict[str, threading.Lock] = {}
mtime_hash_lookup: Dict[str, Dict[str, Any]] = {}

if command_line_args.config_file:
    config_file = command_line_args.config_file
else:
    config_file = os.path.join(os.path.dirname(__file__), "iwyu_config.yml")

with open(config_file, "r") as stream:
    config = yaml.safe_load(stream)
    for key, value in config.items():
        if value is None:
            config[key] = []

IWYU_OPTIONS = config.get("iwyu_options", [])
IWYU_FIX_OPTIONS = config.get("fix_options", [])
NO_INCLUDES = config.get("no_includes", [])
KEEP_INCLUDES = config.get("keep_includes", [])
SKIP_FILES = tuple(config.get("skip_files", []))
CYCLE_FILES: List[str] = []


@dataclass
class CompileCommand:
    """An entry from compile_commands.json."""

    file: str
    command: str
    directory: str
    output: str


class ResultType(enum.Enum):
    """
    Descriptions of enums.

    ERROR: unexpected or unrecognized error cases
    FAILED: the IWYU task for a given compile command entry failed
    NO_CHANGE: the input header tree and source file have not changed since last time
    NOT_RUNNING: sources which we intentionally skip running IWYU all together
    RESUBMIT: the IWYU task failed, but it may work later after other header changes
    SUCCESS: the IWYU task for a source file has succeeded
    """

    ERROR = enum.auto()
    FAILED = enum.auto()
    NO_CHANGE = enum.auto()
    NOT_RUNNING = enum.auto()
    RESUBMIT = enum.auto()
    SUCCESS = enum.auto()


TOOLCHAIN_DIR = command_line_args.mongo_toolchain_bin_dir
SHUTDOWN_FLAG = False
CLANG_INCLUDES = None
IWYU_OPTIONS = [val for pair in zip(["-Xiwyu"] * len(IWYU_OPTIONS), IWYU_OPTIONS) for val in pair]
if NO_INCLUDES:
    NO_INCLUDE_REGEX = re.compile(r"^\s*#include\s+[\",<](" + "|".join(NO_INCLUDES) + ')[",>]')
if KEEP_INCLUDES:
    KEEP_INCLUDE_REGEX = re.compile(r"^\s*#include\s+(" + "|".join(KEEP_INCLUDES) + ")")
CHANGED_FILES_REGEX = re.compile(r"^The\sfull\sinclude-list\sfor\s(.+):$", re.MULTILINE)


def printer(message: str) -> None:
    """
    Prints output as appropriate.

    We don't print output if we are shutting down because the logs will
    explode and original error will be hard to locate.
    """

    if not SHUTDOWN_FLAG or command_line_args.verbose:
        tqdm.write(str(message))


def debug_printer(message: str) -> None:
    """Print each step in the processing of IWYU."""

    if command_line_args.verbose:
        tqdm.write(str(message))


def failed_return() -> ResultType:
    """A common method to allow the processing to continue even after some file fails."""

    if command_line_args.keep_going:
        return ResultType.RESUBMIT
    else:
        return ResultType.FAILED


def in_project_root(file: str) -> bool:
    """
    Return true if the file is in the project root.

    This is assuming the project root is the same location
    as the compile_commands.json file (the format of compile_commands.json
    expects this as well).
    """

    return os.path.abspath(file).startswith(
        os.path.abspath(os.path.dirname(command_line_args.compile_commands))
    )


def copy_error_state(
    cmd_entry: CompileCommand, test_dir: str, dir_ext: str = ".iwyu_test_dir"
) -> Optional[str]:
    """
    When we fail, we want to copy the current state of the temp dir.

    This is so that the command that was used can be replicated and rerun,
    primarily for debugging purposes.
    """

    # we never use a test_dir in check mode, since no files are copied in that mode.
    if command_line_args.check:
        return None

    # make a directory in the output location that we can store the state of the the
    # header dep and source file the compile command was run with, delete old results
    base, _ = os.path.splitext(cmd_entry.output)
    if os.path.exists(base + dir_ext):
        shutil.rmtree(base + dir_ext)
    os.makedirs(base + dir_ext, exist_ok=True)
    basedir = os.path.basename(test_dir)
    error_state_dir = os.path.join(base + dir_ext, basedir)
    shutil.copytree(test_dir, error_state_dir)
    return error_state_dir


def calc_hash_of_file(file: str) -> Optional[str]:
    """
    Calculate the hash of a file. Use mtime as well.

    If the mtime is unchanged, don't do IO, just look up the last hash.
    """

    # we need to lock on specific file io because GIL does not cover system io, so two threads
    # could be doing io on the same file at the same time.
    if file not in hash_lookup_locks:
        hash_lookup_locks[file] = threading.Lock()
    with hash_lookup_locks[file]:
        if file in mtime_hash_lookup and os.path.getmtime(file) == mtime_hash_lookup[file]["mtime"]:
            return mtime_hash_lookup[file]["hash"]
        else:
            try:
                hash_val = hashlib.md5(open(file, "rb").read()).hexdigest()
            except FileNotFoundError:
                return None

            mtime_hash_lookup[file] = {"mtime": os.path.getmtime(file), "hash": hash_val}
            return hash_val


def find_no_include(line: str, lines: List[str], output_lines: List[str]) -> bool:
    """
    We need to regex the line to see if it includes an include that matches our NO_INCLUDE_REGEX.

    If so then we do not include that line
    when we rewrite the file, and instead we add a IWYU no_include pragma inplace
    """

    no_include_header_found = False
    if "// IWYU pragma: keep" in line:
        return no_include_header_found
    no_include_header = re.findall(NO_INCLUDE_REGEX, line)

    if no_include_header:
        no_include_header_found = True
        no_include_line = f'// IWYU pragma: no_include "{no_include_header[0]}"\n'
        if no_include_line not in lines:
            output_lines.append(no_include_line)
    return no_include_header_found


def add_pragmas(source_files: List[str]):
    """
    We automate some of the pragmas so there is not so much manual work.

    There are general cases for some of the pragmas. In this case we open the target
    source/header, search via regexes for specific includes we care about, then add
    the pragma comments as necessary.
    """

    for source_file in source_files:
        # before we run IWYU, we take a guess at the likely header by swapping .cpp for .h
        # so it may not be a real header. After IWYU runs we know exactly where to add the pragmas
        # in case we got it wrong the first time around
        if not os.path.exists(source_file):
            continue

        # we load in the file content operate on it, and then write it back out
        output_lines: List[str] = []
        with open(source_file, "r") as fin:
            file_lines = fin.readlines()
            for line in file_lines:
                if NO_INCLUDES and find_no_include(line, file_lines, output_lines):
                    continue

                if (
                    KEEP_INCLUDES
                    and re.search(KEEP_INCLUDE_REGEX, line)
                    and "// IWYU pragma: keep" not in line
                ):
                    output_lines.append(line.strip() + " // IWYU pragma: keep\n")
                    continue

                output_lines.append(line)

        with open(source_file, "w") as fout:
            for line in output_lines:
                fout.write(line)


def recalc_hashes(deps: List[str], change_dir: Optional[str] = None) -> Dict[str, Any]:
    """
    We calculate the hashes from the header dep list generated by the compiler.

    We also create cumulative hash for convenance.

    Some cases we are operating a test directory, but deps are referenced as if they are
    in the project root. The change_dir option here allows us to calc the the hashes from
    the test directory we may be working in, but still record the deps files in a compat
    fashion with other processes that work out of project root, e.g. testing if there was a
    change from last time.
    """

    hashes: Dict[str, Any] = {"deps": {}}
    full_hash = hashlib.new("md5")
    for dep in sorted(list(deps)):
        if not in_project_root(dep):
            continue
        if change_dir:
            orig_dep = dep
            dep = os.path.join(change_dir, dep)
        dep_hash = calc_hash_of_file(dep)
        if dep_hash is None:
            continue
        if change_dir:
            dep = orig_dep
        full_hash.update(dep_hash.encode("utf-8"))
        hashes["deps"][dep] = dep_hash
    hashes["full_hash"] = full_hash.hexdigest()
    return hashes


def setup_test_dir(cmd_entry: CompileCommand, test_dir: str) -> List[str]:
    """
    Here we are copying the source and required header tree from the main source tree.

    Returns the associate source and header that were copied into the test dir.

    We want an isolated location to perform analysis and apply changes so everything is not
    clashing. At this point we don't know for sure what header IWYU is going to associate with the source
    but for mongo codebase, 99.9% of the time its just swap the .cpp for .h. We need this to apply
    some pragma to keep IWYU from removing headers it doesn't understand (cross platform or
    third party like boost or asio). The pragmas are harmless in and of themselves so adding the
    mistakenly in the 0.1% of the time is negligible.
    """

    original_sources = [
        orig_source
        for orig_source in [cmd_entry.file, os.path.splitext(cmd_entry.file)[0] + ".h"]
        if os.path.exists(orig_source)
    ]
    test_source_files = [os.path.join(test_dir, source_file) for source_file in original_sources]
    dep_headers = [dep for dep in IWYU_ANALYSIS_STATE[cmd_entry.file]["hashes"]["deps"].keys()]

    # copy each required header from our source tree into our test dir
    # this does cost some time, but the alternative (everything operating in the real source tree)
    # was much longer due to constant failures.
    for source_file in dep_headers + ["etc/iwyu_mapping.imp"]:
        if in_project_root(source_file):
            os.makedirs(os.path.join(test_dir, os.path.dirname(source_file)), exist_ok=True)
            shutil.copyfile(source_file, os.path.join(test_dir, source_file))

    # need to create dirs for outputs
    for output in shlex.split(cmd_entry.output):
        os.makedirs(os.path.join(test_dir, os.path.dirname(output)), exist_ok=True)

    return test_source_files


def get_clang_includes() -> List[str]:
    """
    IWYU needs some extra help to know what default includes clang is going to bring in when it normally compiles.

    The query reliably gets the include dirs that would be used in normal compiles. We cache and reuse the result
    so the subprocess only runs once.
    """
    global CLANG_INCLUDES  # pylint: disable=global-statement
    if CLANG_INCLUDES is None:
        clang_includes = subprocess.getoutput(
            f"{TOOLCHAIN_DIR}/clang++ -Wp,-v -x c++ - -fsyntax-only < /dev/null 2>&1 | sed -e '/^#include <...>/,/^End of search/{{ //!b }};d'"
        ).split("\n")
        clang_includes = ["-I" + include.strip() for include in clang_includes]
        CLANG_INCLUDES = clang_includes
    return CLANG_INCLUDES


def write_cycle_diff(source_file: str, cycle_dir: str, latest_hashes: Dict[str, Any]) -> None:
    """
    Write out the diffs between the last iteration and the latest iteration.

    The file contains the hash for before and after for each file involved in the compilation.
    """

    with open(os.path.join(cycle_dir, "hashes_diff.txt"), "w") as out:
        dep_list = set(
            list(IWYU_ANALYSIS_STATE[source_file]["hashes"]["deps"].keys())
            + list(latest_hashes["deps"].keys())
        )
        not_found_str = "not found" + (" " * 23)
        for dep in sorted(dep_list):
            out.write(
                f"Original: {IWYU_ANALYSIS_STATE[source_file]['hashes']['deps'].get(dep, not_found_str)}, Latest: {latest_hashes['deps'].get(dep, not_found_str)} - {dep}\n"
            )


def check_for_cycles(
    cmd_entry: CompileCommand, latest_hashes: Dict[str, Any], test_dir: str
) -> Optional[ResultType]:
    """
    IWYU can induce cycles so we should check our previous results to see if a cycle has occurred.

    These cycles can happen if a header change induces some other header change which then inturn induces
    the original header change. These cycles are generally harmless and are easily broken with a keep
    pragma but finding what files are induces the cycle is the challenge.

    With cycle debug mode enabled, the entire header tree is saved for each iteration in the cycle so
    all files can be fully examined.
    """

    if cmd_entry.file not in IWYU_CYCLE_STATE:
        IWYU_CYCLE_STATE[cmd_entry.file] = {
            "cycles": [],
        }

    if latest_hashes["full_hash"] in IWYU_CYCLE_STATE[cmd_entry.file]["cycles"]:
        if command_line_args.cycle_debugging:
            if "debug_cycles" not in IWYU_CYCLE_STATE[cmd_entry.file]:
                IWYU_CYCLE_STATE[cmd_entry.file]["debug_cycles"] = {}

            IWYU_CYCLE_STATE[cmd_entry.file]["debug_cycles"][latest_hashes["full_hash"]] = (
                latest_hashes
            )

            cycle_dir = copy_error_state(
                cmd_entry,
                test_dir,
                dir_ext=f".{latest_hashes['full_hash']}.cycle{len(IWYU_CYCLE_STATE[cmd_entry.file]['debug_cycles'])}",
            )
            write_cycle_diff(cmd_entry.file, cycle_dir, latest_hashes)
            if latest_hashes["full_hash"] not in IWYU_CYCLE_STATE[cmd_entry.file]["debug_cycles"]:
                printer(f"{Fore.YELLOW}[5] - Cycle Found!: {cmd_entry.file}{Fore.RESET}")
            else:
                printer(f"{Fore.RED}[5] - Cycle Done! : {cmd_entry.file}{Fore.RESET}")
                return failed_return()
        else:
            printer(f"{Fore.RED}[5] - Cycle Found!: {cmd_entry.file}{Fore.RESET}")
            CYCLE_FILES.append(cmd_entry.file)
            return ResultType.SUCCESS
    else:
        IWYU_CYCLE_STATE[cmd_entry.file]["cycles"].append(latest_hashes["full_hash"])

    return None


def write_iwyu_data() -> None:
    """Store the data we have acquired during this run so we can resume at the same spot on subsequent runs."""

    # There might be faster ways to store this like serialization or
    # what not, but having human readable json is good for debugging.
    # on a full build this takes around 10 seconds to write out.
    if IWYU_ANALYSIS_STATE:
        try:
            # atomic move operation prevents ctrl+c mashing from
            # destroying everything, at least we can keep the original
            # data safe from emotional outbursts.
            with tempfile.NamedTemporaryFile() as temp:
                with open(temp.name, "w") as iwyu_data_file:
                    json.dump(IWYU_ANALYSIS_STATE, iwyu_data_file, sort_keys=True, indent=4)
                shutil.move(temp.name, command_line_args.iwyu_data)
        except FileNotFoundError as exc:
            if temp.name in str(exc):
                pass


def need_to_process(
    cmd_entry: CompileCommand, custom_printer: Callable[[str], None] = printer
) -> Optional[ResultType]:
    """
    The first step in the first step for processing a given source file.

    We have a list of skip prefixes, for example build or third_party, but others can be added.

    If it is a file we are not skipping, then we check if we have already done the work by calculating the
    hashes and seeing if what we recorded last time has changed.
    """

    if (
        cmd_entry.file.startswith(SKIP_FILES)
        or cmd_entry.file in CYCLE_FILES
        or "/conftest_" in cmd_entry.file
    ):
        custom_printer(f"{Fore.YELLOW}[5] - Not running!: {cmd_entry.file}{Fore.RESET}")
        return ResultType.NOT_RUNNING

    if IWYU_ANALYSIS_STATE.get(cmd_entry.file):
        hashes = recalc_hashes(IWYU_ANALYSIS_STATE[cmd_entry.file]["hashes"]["deps"].keys())

        # we only skip if the matching mode was successful last time, otherwise we assume we need to rerun
        mode_success = "CHECK" if command_line_args.check else "FIX"
        if command_line_args.verbose:
            diff_files = list(
                set(hashes["deps"].keys()).symmetric_difference(
                    set(IWYU_ANALYSIS_STATE[cmd_entry.file]["hashes"]["deps"].keys())
                )
            )
            if diff_files:
                msg = f"[1] Need to process {cmd_entry.file} because different files:\n"
                for file in diff_files:
                    msg += f"{file}\n"
                debug_printer(msg)
            for file in IWYU_ANALYSIS_STATE[cmd_entry.file]["hashes"]["deps"].keys():
                if (
                    file in hashes["deps"]
                    and hashes["deps"][file]
                    != IWYU_ANALYSIS_STATE[cmd_entry.file]["hashes"]["deps"][file]
                ):
                    debug_printer(
                        f"[1] Need to process {cmd_entry.file} because hash changed:\n{file}: {hashes['deps'][file]}\n{file}: {IWYU_ANALYSIS_STATE[cmd_entry.file]['hashes']['deps'][file]}"
                    )

        if hashes["full_hash"] == IWYU_ANALYSIS_STATE[cmd_entry.file]["hashes"][
            "full_hash"
        ] and mode_success in IWYU_ANALYSIS_STATE[cmd_entry.file].get("success", []):
            custom_printer(f"{Fore.YELLOW}[5] - No Change!  : {cmd_entry.file}{Fore.RESET}")
            return ResultType.NO_CHANGE

    return None


def calc_dep_headers(cmd_entry: CompileCommand) -> Optional[ResultType]:
    """
    The second step in the IWYU process.

    We need to get a list of headers which are dependencies so we can copy them to an isolated
    working directory (so parallel IWYU changes don't break us). We will switch on preprocessor
    for faster generation of the dep file.

    Once we have the deps list, we parse it and calc the hashes of the deps.
    """

    try:
        with tempfile.NamedTemporaryFile() as depfile:
            # first time we could be executing a real command so we make sure the dir
            # so the compiler is not mad
            outputs = shlex.split(cmd_entry.output)
            for output in outputs:
                out_dir = os.path.dirname(output)
                if out_dir:
                    os.makedirs(out_dir, exist_ok=True)

            # setup up command for fast depfile generation
            cmd = cmd_entry.command
            cmd += f" -MD -MF {depfile.name}"
            cmd = cmd.replace(" -c ", " -E ")
            debug_printer(f"[1] - Getting Deps: {cmd_entry.file}")

            try:
                deps_proc = subprocess.run(
                    cmd, shell=True, capture_output=True, text=True, timeout=300
                )
            except subprocess.TimeoutExpired:
                deps_proc = None
                pass

            # if successful, record the latest deps with there hashes, otherwise try again later
            if deps_proc is None or deps_proc.returncode != 0:
                printer(f"{Fore.RED}[5] - Deps Failed!: {cmd_entry.file}{Fore.RESET}")
                printer(deps_proc.stderr)
                return ResultType.RESUBMIT
            else:
                with open(depfile.name) as deps:
                    deps_str = deps.read()
                    deps_str = deps_str.replace("\\\n", "").strip()

                    hashes = recalc_hashes(shlex.split(deps_str)[1:])
                    if not IWYU_ANALYSIS_STATE.get(cmd_entry.file):
                        IWYU_ANALYSIS_STATE[cmd_entry.file] = asdict(cmd_entry)
                    IWYU_ANALYSIS_STATE[cmd_entry.file]["hashes"] = hashes
                    IWYU_ANALYSIS_STATE[cmd_entry.file]["success"] = []

    # if the dep command failed the context will through an execption, we will ignore just
    # that case
    except FileNotFoundError as exc:
        traceback.print_exc()
        if depfile.name in str(exc):
            pass

    return None


def execute_iwyu(cmd_entry: CompileCommand, test_dir: str) -> Union[ResultType, bytes]:
    """
    The third step of IWYU analysis. Check mode will stop here.

    Here we want to execute IWYU on our source. Note at this point in fix mode
    we will be working out of an isolated test directory which has the
    required header tree copied over. Check mode will just pass in the original
    project root as the test_dir (the real source tree).
    """

    # assert we are working with a pure clang++ build
    if not cmd_entry.command.startswith(f"{TOOLCHAIN_DIR}/clang++"):
        printer("unexpected compiler:")
        printer(cmd_entry.command)
        return ResultType.FAILED

    # swap out for our tool and add in extra options for IWYU
    cmd = (
        f"{TOOLCHAIN_DIR}/include-what-you-use"
        + cmd_entry.command[len(f"{TOOLCHAIN_DIR}/clang++") :]
    )
    cmd += " " + " ".join(get_clang_includes())
    cmd += " " + " ".join(IWYU_OPTIONS)

    # mimic the PATH we normally use in our build
    env = os.environ.copy()
    env["PATH"] += f":{TOOLCHAIN_DIR}"

    debug_printer(f"[2] - Running IWYU: {cmd_entry.file}")
    proc = subprocess.run(cmd, shell=True, env=env, capture_output=True, cwd=test_dir)

    # IWYU has some bugs about forward declares I am assuming, because in some cases even though
    # we have passed --no_fwd_decls it still sometimes recommend forward declares and sometimes they
    # are wrong and cause compilation errors.
    remove_fwd_declares = []
    for line in proc.stderr.decode("utf-8").split("\n"):
        line = line.strip()
        if (
            not line.endswith(":")
            and not line.startswith(("#include ", "-"))
            and ("class " in line or "struct " in line)
        ):
            continue
        remove_fwd_declares.append(line)
    iwyu_output = "\n".join(remove_fwd_declares)

    # IWYU has weird exit codes, where a >=2 is considered success:
    # https://github.com/include-what-you-use/include-what-you-use/blob/clang_12/iwyu_globals.h#L27-L34
    if command_line_args.check and proc.returncode != 2:
        printer(f"{Fore.RED}[2] - IWYU Failed: {cmd_entry.file}{Fore.RESET}")
        if proc.returncode < 2:
            printer(f"exited with error: {proc.returncode}")
        else:
            printer(f"changes required: {proc.returncode - 2}")
        printer(iwyu_output)
        return failed_return()
    elif proc.returncode < 2:
        printer(f"{Fore.RED}[2] - IWYU Failed : {cmd_entry.file}{Fore.RESET}")
        printer(cmd)
        printer(str(proc.returncode))
        printer(proc.stderr.decode("utf-8"))
        copy_error_state(cmd_entry, test_dir)
        return failed_return()

    # save the output for debug or inspection later
    with open(os.path.splitext(cmd_entry.output)[0] + ".iwyu", "w") as iwyu_out:
        iwyu_out.write(iwyu_output)

    return iwyu_output.encode("utf-8")


def apply_fixes(
    cmd_entry: CompileCommand, iwyu_output: bytes, test_dir: str
) -> Optional[ResultType]:
    """
    Step 4 in the IWYU process.

    We need to run the fix_includes script to apply the output from the IWYU binary.
    """
    cmd = [f"{sys.executable}", f"{TOOLCHAIN_DIR}/fix_includes.py"] + IWYU_FIX_OPTIONS

    debug_printer(f"[3] - Apply fixes : {cmd_entry.file}")
    try:
        subprocess.run(cmd, capture_output=True, input=iwyu_output, timeout=180, cwd=test_dir)
    except subprocess.TimeoutExpired:
        printer(f"{Fore.RED}[5] - Apply failed: {cmd_entry.file}{Fore.RESET}")
        return ResultType.RESUBMIT

    return None


def test_compile(cmd_entry: CompileCommand, test_dir: str) -> Optional[ResultType]:
    """
    Step 5 in the IWYU analysis and the last step for fix mode.

    We run the normal compile command in a test directory and make sure it is successful before
    it will be copied back into the real source tree for inclusion into other jobs.
    """

    try:
        with tempfile.NamedTemporaryFile() as depfile:
            debug_printer(f"[4] - Test compile: {cmd_entry.file}")

            # we want to capture the header deps again because IWYU may have changed them
            cmd = cmd_entry.command
            cmd += f" -MMD -MF {depfile.name}"
            try:
                p3 = subprocess.run(
                    cmd, shell=True, capture_output=True, text=True, timeout=300, cwd=test_dir
                )
            except (subprocess.TimeoutExpired, MemoryError):
                p3 = None
                pass

            # our test compile has failed so we need to report and setup for debug
            if p3 is not None and p3.returncode != 0:
                printer(f"{Fore.RED}[5] - IWYU Failed!: {cmd_entry.file}{Fore.RESET}")
                printer(f"{cmd}")
                printer(f"{p3.stderr}")
                copy_error_state(cmd_entry, test_dir)
                return failed_return()

            else:
                with open(depfile.name) as deps:
                    # calculate the hashes of the deps used to create
                    # this successful compile.
                    deps_str = deps.read()
                    deps_str = deps_str.replace("\\\n", "").strip()
                    hashes = recalc_hashes(shlex.split(deps_str)[1:], change_dir=test_dir)

                    if result := check_for_cycles(cmd_entry, hashes, test_dir):
                        return result

                    IWYU_ANALYSIS_STATE[cmd_entry.file]["hashes"] = hashes
                    if "FIX" not in IWYU_ANALYSIS_STATE[cmd_entry.file]["success"]:
                        IWYU_ANALYSIS_STATE[cmd_entry.file]["success"].append("FIX")
                    printer(f"{Fore.GREEN}[5] - IWYU Success: {cmd_entry.file}{Fore.RESET}")
                    return ResultType.SUCCESS

    # if we failed, the depfile may not have been generated, so check for it
    # ignore it
    except FileNotFoundError as exc:
        if depfile.name in str(exc):
            pass

    return None


def intialize_deps(cmd_entry: CompileCommand) -> Tuple[ResultType, CompileCommand]:
    """
    When running in fix mode, we take some time to initialize the header deps.

    This is mainly used to improve the overall time to complete full analysis. We want process
    the source files in order of files with least dependencies to most dependencies. The rational
    is that if it has a lot of dependencies we should do last so any changes in those dependencies
    are automatically accounted for and the change of need to do rework is lessened. Also the
    progress bar can be more accurate and not count skip files.
    """

    # step 1
    if result := need_to_process(cmd_entry, custom_printer=debug_printer):
        return result, cmd_entry

    # if we have deps from a previous that should be a good enough indicator
    # of how dependency heavy it is, and its worth just taking that over
    # needing to invoke the compiler.
    try:
        if len(IWYU_ANALYSIS_STATE[cmd_entry.file]["hashes"]["deps"]):
            return ResultType.SUCCESS, cmd_entry

    except KeyError:
        pass

    if result := calc_dep_headers(cmd_entry):
        return result, cmd_entry

    return ResultType.SUCCESS, cmd_entry


def check_iwyu(cmd_entry: CompileCommand) -> ResultType:
    """
    One of the two thread functions the main thread pool executor will call.

    Here we execute up to step 3 (steps at the top comment) and report success
    if IWYU reports no required changes.
    """

    # step 1
    if result := need_to_process(cmd_entry):
        return result

    # step 2
    if result := calc_dep_headers(cmd_entry):
        return result

    # step 3
    iwyu_out = execute_iwyu(cmd_entry, ".")
    if isinstance(iwyu_out, ResultType):
        return iwyu_out

    # success!
    printer(f"{Fore.GREEN}[2] - IWYU Success: {cmd_entry.file}{Fore.RESET}")
    if "CHECK" not in IWYU_ANALYSIS_STATE[cmd_entry.file]["success"]:
        IWYU_ANALYSIS_STATE[cmd_entry.file]["success"].append("CHECK")
    return ResultType.SUCCESS


def fix_iwyu(cmd_entry: CompileCommand) -> ResultType:
    """
    One of the two thread functions the main thread pool executor will call.

    Here we execute up to step 5 (steps at the top comment) and report success
    if we are able to successfully compile the original command after IWYU
    has made its changes.
    """

    # step 1
    if result := need_to_process(cmd_entry):
        return result

    # step 2
    if result := calc_dep_headers(cmd_entry):
        return result

    with tempfile.TemporaryDirectory() as test_dir:
        # the changes will be done in an isolated test dir so not to conflict with
        # other concurrent processes.
        test_source_files = setup_test_dir(cmd_entry, test_dir)

        # a first round of pragmas to make sure IWYU doesn't fail or remove things we dont want
        add_pragmas(test_source_files)

        # step 3
        iwyu_out = execute_iwyu(cmd_entry, test_dir)
        if isinstance(iwyu_out, ResultType):
            return iwyu_out

        # now we can extract exactly what files IWYU operated on and copy only those back
        changed_files = [
            os.path.join(test_dir, file)
            for file in re.findall(CHANGED_FILES_REGEX, iwyu_out.decode("utf-8"))
            if in_project_root(file)
        ]
        test_source_files += [file for file in changed_files if file not in test_source_files]

        # step 4
        if result := apply_fixes(cmd_entry, iwyu_out, test_dir):
            return result

        # a final round of pragmas for the next time this is run through IWYU
        add_pragmas(test_source_files)

        # step 5
        result = test_compile(cmd_entry, test_dir)
        if result == ResultType.SUCCESS:
            for file in test_source_files:
                if os.path.exists(file):
                    shutil.move(file, file[len(test_dir) + 1 :])

        return result


def run_iwyu(cmd_entry: CompileCommand) -> Tuple[ResultType, CompileCommand]:
    """Intermediate function which delegates the underlying mode to run."""

    if command_line_args.check:
        return check_iwyu(cmd_entry), cmd_entry
    else:
        return fix_iwyu(cmd_entry), cmd_entry


def main() -> None:
    """Main function."""
    global IWYU_ANALYSIS_STATE, SHUTDOWN_FLAG  # pylint: disable=global-statement
    atexit.register(write_iwyu_data)

    with concurrent.futures.ThreadPoolExecutor(
        max_workers=len(os.sched_getaffinity(0)) + 4
    ) as executor:
        # ctrl+c tru to shutdown as fast as possible.
        def sigint_handler(the_signal, frame):
            executor.shutdown(wait=False, cancel_futures=True)
            sys.exit(1)

        signal.signal(signal.SIGINT, sigint_handler)

        # load in any data from prior runs
        if os.path.exists(command_line_args.iwyu_data):
            with open(command_line_args.iwyu_data) as iwyu_data_file:
                IWYU_ANALYSIS_STATE = json.load(iwyu_data_file)

        # load in the compile commands
        with open(command_line_args.compile_commands) as compdb_file:
            compiledb = [CompileCommand(**json_data) for json_data in json.load(compdb_file)]

            # assert the generated source code has been generated
            for cmd_entry in compiledb:
                if cmd_entry.file.endswith("_gen.cpp") and not os.path.exists(cmd_entry.file):
                    printer(f"{Fore.RED}[5] - Missing Gen!: {cmd_entry.file}{Fore.RESET}")
                    printer(
                        f"Error: missing generated file {cmd_entry.file}, make sure generated-sources are generated."
                    )
                    sys.exit(1)

            total_cmds = len(compiledb)
            start_index = int(total_cmds * command_line_args.start_ratio)
            if start_index < 0:
                start_index = 0
            if start_index > total_cmds:
                start_index = total_cmds

            end_index = int(total_cmds * command_line_args.end_ratio)
            if end_index < 0:
                end_index = 0
            if end_index > total_cmds:
                end_index = total_cmds

            if start_index == end_index:
                print(f"Error: start_index and end_index are the same: {start_index}")
                sys.exit(1)
            if start_index > end_index:
                print(
                    f"Error: start_index {start_index} can not be greater than end_index {end_index}"
                )
                sys.exit(1)

            print(f"Analyzing compile commands from {start_index} to {end_index}.")
            compiledb = compiledb[start_index:end_index]
            if not command_line_args.check:
                # We can optimize the order we process things by processing source files
                # with the least number of dependencies first. This is a cost up front
                # but will result in huge gains in the amount of re-processing to be done.
                printer("Getting Initial Header Dependencies...")
                cmd_entry_list = []
                try:
                    with tqdm(total=len(compiledb), disable=None) as pbar:
                        # create and run the dependency check jobs
                        future_cmd = {
                            executor.submit(intialize_deps, cmd_entry): cmd_entry
                            for cmd_entry in compiledb
                        }
                        for future in concurrent.futures.as_completed(future_cmd):
                            result, cmd_entry = future.result()
                            if result != ResultType.NOT_RUNNING:
                                cmd_entry_list.append(cmd_entry)
                            pbar.update(1)
                except Exception:
                    SHUTDOWN_FLAG = True
                    traceback.print_exc()
                    executor.shutdown(wait=True, cancel_futures=True)
                    sys.exit(1)
            else:
                cmd_entry_list = compiledb

            try:
                # this loop will keep looping until a full run produce no new changes.
                changes_left = True
                while changes_left:
                    changes_left = False

                    with tqdm(total=len(cmd_entry_list), disable=None) as pbar:
                        # create and run the IWYU jobs
                        def dep_sorted(cmd_entry):
                            try:
                                return len(IWYU_ANALYSIS_STATE[cmd_entry.file]["hashes"]["deps"])
                            except KeyError:
                                return 0

                        future_cmd = {
                            executor.submit(run_iwyu, cmd_entry): cmd_entry
                            for cmd_entry in sorted(cmd_entry_list, key=dep_sorted)
                        }

                        # process the results
                        for future in concurrent.futures.as_completed(future_cmd):
                            result, cmd_entry = future.result()

                            # any result which implies there could be changes required sets the
                            # next loop
                            if result not in (ResultType.NO_CHANGE, ResultType.NOT_RUNNING):
                                changes_left = True

                            # if a file is considered done for this loop, update the status bar
                            if result in [
                                ResultType.SUCCESS,
                                ResultType.NO_CHANGE,
                                ResultType.NOT_RUNNING,
                            ]:
                                pbar.update(1)
                            # resubmit jobs which may have a better change to run later
                            elif result == ResultType.RESUBMIT:
                                executor.submit(run_iwyu, cmd_entry)
                            # handle a failure case, excpetion quickly drops us out of this loop.
                            else:
                                SHUTDOWN_FLAG = True
                                tqdm.write(
                                    f"{result.name}: Shutting down other threads, please be patient."
                                )
                                raise Exception(
                                    f'Shutdown due to {result.name} {cmd_entry["file"]}'
                                )

            except Exception:
                SHUTDOWN_FLAG = True
                traceback.print_exc()
                executor.shutdown(wait=True, cancel_futures=True)
                sys.exit(1)
            finally:
                if CYCLE_FILES:
                    printer(f"{Fore.YELLOW} Cycles detected:")
                    for file in CYCLE_FILES:
                        printer(f"    {file}")


main()
