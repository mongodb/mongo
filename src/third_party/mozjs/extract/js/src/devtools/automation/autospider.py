#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this file,
# You can obtain one at http://mozilla.org/MPL/2.0/.


import argparse
import json
import logging
import multiprocessing
import re
import os
import platform
import posixpath
import shlex
import shutil
import subprocess
import sys

from collections import Counter, namedtuple
from logging import info
from os import environ as env
from pathlib import Path
from subprocess import Popen
from threading import Timer

Dirs = namedtuple("Dirs", ["scripts", "js_src", "source", "fetches"])


def directories(pathmodule, cwd, fixup=lambda s: s):
    scripts = pathmodule.join(fixup(cwd), fixup(pathmodule.dirname(__file__)))
    js_src = pathmodule.abspath(pathmodule.join(scripts, "..", ".."))
    source = pathmodule.abspath(pathmodule.join(js_src, "..", ".."))
    mozbuild = pathmodule.abspath(
        # os.path.expanduser does not work on Windows.
        env.get("MOZBUILD_STATE_PATH")
        or pathmodule.join(Path.home(), ".mozbuild")
    )
    fetches = pathmodule.abspath(env.get("MOZ_FETCHES_DIR", mozbuild))
    return Dirs(scripts, js_src, source, fetches)


def quote(s):
    # shlex quotes for the purpose of passing to the native shell, which is cmd
    # on Windows, and therefore will not replace backslashed paths with forward
    # slashes. When such a path is passed to sh, the backslashes will be
    # interpreted as escape sequences.
    return shlex.quote(s).replace("\\", "/")


# Some scripts will be called with sh, which cannot use backslashed
# paths. So for direct subprocess.* invocation, use normal paths from
# DIR, but when running under the shell, use POSIX style paths.
DIR = directories(os.path, os.getcwd())
PDIR = directories(
    posixpath, os.environ["PWD"], fixup=lambda s: re.sub(r"^(\w):", r"/\1", s)
)

AUTOMATION = env.get("AUTOMATION", False)

parser = argparse.ArgumentParser(description="Run a spidermonkey shell build job")
parser.add_argument(
    "--verbose",
    action="store_true",
    default=AUTOMATION,
    help="display additional logging info",
)
parser.add_argument(
    "--dep", action="store_true", help="do not clobber the objdir before building"
)
parser.add_argument(
    "--keep",
    action="store_true",
    help="do not delete the sanitizer output directory (for testing)",
)
parser.add_argument(
    "--platform",
    "-p",
    type=str,
    metavar="PLATFORM",
    default="",
    help='build platform, including a suffix ("-debug" or "") used '
    'by buildbot to override the variant\'s "debug" setting. The platform can be '
    "used to specify 32 vs 64 bits.",
)
parser.add_argument(
    "--timeout",
    "-t",
    type=int,
    metavar="TIMEOUT",
    default=12600,
    help="kill job after TIMEOUT seconds",
)
parser.add_argument(
    "--objdir",
    type=str,
    metavar="DIR",
    # The real default must be set later so that OBJDIR and POBJDIR can be
    # platform-dependent strings.
    default=env.get("OBJDIR"),
    help="object directory",
)
group = parser.add_mutually_exclusive_group()
group.add_argument(
    "--optimize",
    action="store_true",
    help="generate an optimized build. Overrides variant setting.",
)
group.add_argument(
    "--no-optimize",
    action="store_false",
    dest="optimize",
    help="generate a non-optimized build. Overrides variant setting.",
)
group.set_defaults(optimize=None)
group = parser.add_mutually_exclusive_group()
group.add_argument(
    "--debug",
    action="store_true",
    help="generate a debug build. Overrides variant setting.",
)
group.add_argument(
    "--no-debug",
    action="store_false",
    dest="debug",
    help="generate a non-debug build. Overrides variant setting.",
)
group.set_defaults(debug=None)
group = parser.add_mutually_exclusive_group()
group.add_argument(
    "--jemalloc",
    action="store_true",
    dest="jemalloc",
    help="use mozilla's jemalloc instead of the default allocator",
)
group.add_argument(
    "--no-jemalloc",
    action="store_false",
    dest="jemalloc",
    help="use the default allocator instead of mozilla's jemalloc",
)
group.set_defaults(jemalloc=None)
parser.add_argument(
    "--run-tests",
    "--tests",
    type=str,
    metavar="TESTSUITE",
    default="",
    help="comma-separated set of test suites to add to the variant's default set",
)
parser.add_argument(
    "--skip-tests",
    "--skip",
    type=str,
    metavar="TESTSUITE",
    default="",
    help="comma-separated set of test suites to remove from the variant's default "
    "set",
)
parser.add_argument(
    "--build-only",
    "--build",
    dest="skip_tests",
    action="store_const",
    const="all",
    help="only do a build, do not run any tests",
)
parser.add_argument(
    "--nobuild",
    action="store_true",
    help="Do not do a build. Rerun tests on existing build.",
)
parser.add_argument(
    "variant", type=str, help="type of job requested, see variants/ subdir"
)
args = parser.parse_args()

logging.basicConfig(level=logging.INFO, format="%(message)s")

env["CPP_UNIT_TESTS_DIR_JS_SRC"] = DIR.js_src
if AUTOMATION and platform.system() == "Windows":
    # build/win{32,64}/mozconfig.vs-latest uses TOOLTOOL_DIR to set VSPATH.
    env["TOOLTOOL_DIR"] = DIR.fetches

OBJDIR = args.objdir or os.path.join(DIR.source, "obj-spider")
OBJDIR = os.path.abspath(OBJDIR)
OUTDIR = os.path.join(OBJDIR, "out")
POBJDIR = args.objdir or posixpath.join(PDIR.source, "obj-spider")
POBJDIR = posixpath.abspath(POBJDIR)
MAKE = env.get("MAKE", "make")
PYTHON = sys.executable

for d in DIR._fields:
    info("DIR.{name} = {dir}".format(name=d, dir=getattr(DIR, d)))


def ensure_dir_exists(
    name, clobber=True, creation_marker_filename="CREATED-BY-AUTOSPIDER"
):
    if creation_marker_filename is None:
        marker = None
    else:
        marker = os.path.join(name, creation_marker_filename)
    if clobber:
        if (
            not AUTOMATION
            and marker
            and os.path.exists(name)
            and not os.path.exists(marker)
        ):
            raise Exception(
                "Refusing to delete objdir %s because it was not created by autospider"
                % name
            )
        shutil.rmtree(name, ignore_errors=True)
    try:
        os.mkdir(name)
        if marker:
            open(marker, "a").close()
    except OSError:
        if clobber:
            raise


with open(os.path.join(DIR.scripts, "variants", args.variant)) as fh:
    variant = json.load(fh)

if args.variant == "nonunified":
    # Rewrite js/src/**/moz.build to replace UNIFIED_SOURCES to SOURCES.
    # Note that this modifies the current checkout.
    for dirpath, dirnames, filenames in os.walk(DIR.js_src):
        if "moz.build" in filenames:
            in_place = ["-i"]
            if platform.system() == "Darwin":
                in_place.append("")
            subprocess.check_call(
                ["sed"]
                + in_place
                + ["s/UNIFIED_SOURCES/SOURCES/", os.path.join(dirpath, "moz.build")]
            )

CONFIGURE_ARGS = variant["configure-args"]

compiler = variant.get("compiler")
if compiler != "gcc" and "clang-plugin" not in CONFIGURE_ARGS:
    CONFIGURE_ARGS += " --enable-clang-plugin"

if compiler == "gcc":
    if AUTOMATION:
        fetches = env["MOZ_FETCHES_DIR"]
        env["CC"] = os.path.join(fetches, "gcc", "bin", "gcc")
        env["CXX"] = os.path.join(fetches, "gcc", "bin", "g++")
    else:
        env["CC"] = "gcc"
        env["CXX"] = "g++"

opt = args.optimize
if opt is None:
    opt = variant.get("optimize")
if opt is not None:
    CONFIGURE_ARGS += " --enable-optimize" if opt else " --disable-optimize"

opt = args.debug
if opt is None:
    opt = variant.get("debug")
if opt is not None:
    CONFIGURE_ARGS += " --enable-debug" if opt else " --disable-debug"

opt = args.jemalloc
if opt is not None:
    CONFIGURE_ARGS += " --enable-jemalloc" if opt else " --disable-jemalloc"

# By default, we build with NSPR, even if not specified. But we actively allow
# builds to disable NSPR.
opt = variant.get("nspr")
if opt is None or opt:
    CONFIGURE_ARGS += " --enable-nspr-build"

# Some of the variants request a particular word size (eg ARM simulators).
word_bits = variant.get("bits")

# On Linux and Windows, we build 32- and 64-bit versions on a 64 bit
# host, so the caller has to specify what is desired.
if word_bits is None and args.platform:
    platform_arch = args.platform.split("-")[0]
    if platform_arch in ("win32", "linux"):
        word_bits = 32
    elif platform_arch in ("win64", "linux64"):
        word_bits = 64

# Fall back to the word size of the host.
if word_bits is None:
    word_bits = 64 if platform.architecture()[0] == "64bit" else 32

# Need a platform name to use as a key in variant files.
if args.platform:
    variant_platform = args.platform.split("-")[0]
elif platform.system() == "Windows":
    variant_platform = "win64" if word_bits == 64 else "win32"
elif platform.system() == "Linux":
    variant_platform = "linux64" if word_bits == 64 else "linux"
elif platform.system() == "Darwin":
    variant_platform = "macosx64"
else:
    variant_platform = "other"

env["LD_LIBRARY_PATH"] = ":".join(
    d
    for d in [
        # for libnspr etc.
        os.path.join(OBJDIR, "dist", "bin"),
        # existing search path, if any
        env.get("LD_LIBRARY_PATH"),
    ]
    if d is not None
)

os.environ["SOURCE"] = DIR.source
if platform.system() == "Windows":
    MAKE = env.get("MAKE", "mozmake")

# Configure flags, based on word length and cross-compilation
if word_bits == 32:
    if platform.system() == "Windows":
        CONFIGURE_ARGS += " --target=i686-pc-mingw32"
    elif platform.system() == "Linux":
        if not platform.machine().startswith("arm"):
            CONFIGURE_ARGS += " --target=i686-pc-linux"

    # Add SSE2 support for x86/x64 architectures.
    if not platform.machine().startswith("arm"):
        if platform.system() == "Windows":
            sse_flags = "-arch:SSE2"
        else:
            sse_flags = "-msse -msse2 -mfpmath=sse"
        env["CCFLAGS"] = "{0} {1}".format(env.get("CCFLAGS", ""), sse_flags)
        env["CXXFLAGS"] = "{0} {1}".format(env.get("CXXFLAGS", ""), sse_flags)
else:
    if platform.system() == "Windows":
        CONFIGURE_ARGS += " --target=x86_64-pc-mingw32"

if platform.system() == "Linux" and AUTOMATION:
    CONFIGURE_ARGS = "--enable-stdcxx-compat --disable-gold " + CONFIGURE_ARGS

# Timeouts.
ACTIVE_PROCESSES = set()


def killall():
    for proc in ACTIVE_PROCESSES:
        proc.kill()
    ACTIVE_PROCESSES.clear()


timer = Timer(args.timeout, killall)
timer.daemon = True
timer.start()

ensure_dir_exists(OBJDIR, clobber=not args.dep and not args.nobuild)
ensure_dir_exists(OUTDIR, clobber=not args.keep)

# Any jobs that wish to produce additional output can save them into the upload
# directory if there is such a thing, falling back to OBJDIR.
env.setdefault("MOZ_UPLOAD_DIR", OBJDIR)
ensure_dir_exists(env["MOZ_UPLOAD_DIR"], clobber=False, creation_marker_filename=None)
info("MOZ_UPLOAD_DIR = {}".format(env["MOZ_UPLOAD_DIR"]))


def run_command(command, check=False, **kwargs):
    kwargs.setdefault("cwd", OBJDIR)
    info("in directory {}, running {}".format(kwargs["cwd"], command))
    if platform.system() == "Windows":
        # Windows will use cmd for the shell, which causes all sorts of
        # problems. Use sh instead, quoting appropriately. (Use sh in all
        # cases, not just when shell=True, because we want to be able to use
        # paths that sh understands and cmd does not.)
        if not isinstance(command, list):
            if kwargs.get("shell"):
                command = shlex.split(command)
            else:
                command = [command]

        command = " ".join(quote(c) for c in command)
        command = ["sh", "-c", command]
        kwargs["shell"] = False
    proc = Popen(command, **kwargs)
    ACTIVE_PROCESSES.add(proc)
    stdout, stderr = None, None
    try:
        stdout, stderr = proc.communicate()
    finally:
        ACTIVE_PROCESSES.discard(proc)
    status = proc.wait()
    if check and status != 0:
        raise subprocess.CalledProcessError(status, command, output=stderr)
    return stdout, stderr, status


# Replacement strings in environment variables.
REPLACEMENTS = {
    "DIR": DIR.scripts,
    "MOZ_FETCHES_DIR": DIR.fetches,
    "MOZ_UPLOAD_DIR": env["MOZ_UPLOAD_DIR"],
    "OUTDIR": OUTDIR,
}

# Add in environment variable settings for this variant. Normally used to
# modify the flags passed to the shell or to set the GC zeal mode.
for k, v in variant.get("env", {}).items():
    env[k] = v.format(**REPLACEMENTS)

if AUTOMATION:
    # Currently only supported on linux64.
    if platform.system() == "Linux" and word_bits == 64:
        use_minidump = variant.get("use_minidump", True)
    else:
        use_minidump = False
else:
    use_minidump = False


def resolve_path(dirs, *components):
    if None in components:
        return None
    for dir in dirs:
        path = os.path.join(dir, *components)
        if os.path.exists(path):
            return path


if use_minidump:
    env.setdefault("MINIDUMP_SAVE_PATH", env["MOZ_UPLOAD_DIR"])

    injector_basename = {
        "Linux": "libbreakpadinjector.so",
        "Darwin": "breakpadinjector.dylib",
    }.get(platform.system())

    injector_lib = resolve_path((DIR.fetches,), "injector", injector_basename)
    stackwalk = resolve_path((DIR.fetches,), "minidump_stackwalk", "minidump_stackwalk")
    if stackwalk is not None:
        env.setdefault("MINIDUMP_STACKWALK", stackwalk)
    dump_syms = resolve_path((DIR.fetches,), "dump_syms", "dump_syms")
    if dump_syms is not None:
        env.setdefault("DUMP_SYMS", dump_syms)

    if injector_lib is None:
        use_minidump = False

    info("use_minidump is {}".format(use_minidump))
    info("  MINIDUMP_SAVE_PATH={}".format(env["MINIDUMP_SAVE_PATH"]))
    info("  injector lib is {}".format(injector_lib))
    info("  MINIDUMP_STACKWALK={}".format(env.get("MINIDUMP_STACKWALK")))


mozconfig = os.path.join(DIR.source, "mozconfig.autospider")
CONFIGURE_ARGS += " --prefix={OBJDIR}/dist".format(OBJDIR=quote(OBJDIR))

# Generate a mozconfig.
with open(mozconfig, "wt") as fh:
    if AUTOMATION and platform.system() == "Windows":
        fh.write('. "$topsrcdir/build/%s/mozconfig.vs-latest"\n' % variant_platform)
    fh.write("ac_add_options --enable-project=js\n")
    fh.write("ac_add_options " + CONFIGURE_ARGS + "\n")
    fh.write("mk_add_options MOZ_OBJDIR=" + quote(OBJDIR) + "\n")

env["MOZCONFIG"] = mozconfig

mach = posixpath.join(PDIR.source, "mach")

if not args.nobuild:
    # Do the build
    run_command([mach, "build"], check=True)

    if use_minidump:
        # Convert symbols to breakpad format.
        cmd_env = env.copy()
        cmd_env["MOZ_SOURCE_REPO"] = "file://" + DIR.source
        cmd_env["RUSTC_COMMIT"] = "0"
        cmd_env["MOZ_CRASHREPORTER"] = "1"
        cmd_env["MOZ_AUTOMATION_BUILD_SYMBOLS"] = "1"
        run_command(
            [
                mach,
                "build",
                "recurse_syms",
            ],
            check=True,
            env=cmd_env,
        )

COMMAND_PREFIX = []
# On Linux, disable ASLR to make shell builds a bit more reproducible.
if subprocess.call("type setarch >/dev/null 2>&1", shell=True) == 0:
    COMMAND_PREFIX.extend(["setarch", platform.machine(), "-R"])


def run_test_command(command, **kwargs):
    _, _, status = run_command(COMMAND_PREFIX + command, check=False, **kwargs)
    return status


default_test_suites = frozenset(["jstests", "jittest", "jsapitests", "checks"])
nondefault_test_suites = frozenset(["gdb"])
all_test_suites = default_test_suites | nondefault_test_suites

test_suites = set(default_test_suites)


def normalize_tests(tests):
    if "all" in tests:
        return default_test_suites
    return tests


# Override environment variant settings conditionally.
for k, v in variant.get("conditional-env", {}).get(variant_platform, {}).items():
    env[k] = v.format(**REPLACEMENTS)

# Skip any tests that are not run on this platform (or the 'all' platform).
test_suites -= set(
    normalize_tests(variant.get("skip-tests", {}).get(variant_platform, []))
)
test_suites -= set(normalize_tests(variant.get("skip-tests", {}).get("all", [])))

# Add in additional tests for this platform (or the 'all' platform).
test_suites |= set(
    normalize_tests(variant.get("extra-tests", {}).get(variant_platform, []))
)
test_suites |= set(normalize_tests(variant.get("extra-tests", {}).get("all", [])))

# Now adjust the variant's default test list with command-line arguments.
test_suites |= set(normalize_tests(args.run_tests.split(",")))
test_suites -= set(normalize_tests(args.skip_tests.split(",")))
if "all" in args.skip_tests.split(","):
    test_suites = []

# Bug 1391877 - Windows test runs are getting mysterious timeouts when run
# through taskcluster, but only when running multiple jit-test jobs in
# parallel. Work around them for now.
if platform.system() == "Windows":
    env["JITTEST_EXTRA_ARGS"] = "-j1 " + env.get("JITTEST_EXTRA_ARGS", "")

# Bug 1557130 - Atomics tests can create many additional threads which can
# lead to resource exhaustion, resulting in intermittent failures. This was
# only seen on beefy machines (> 32 cores), so limit the number of parallel
# workers for now.
if platform.system() == "Windows":
    worker_count = min(multiprocessing.cpu_count(), 16)
    env["JSTESTS_EXTRA_ARGS"] = "-j{} ".format(worker_count) + env.get(
        "JSTESTS_EXTRA_ARGS", ""
    )

if use_minidump:
    # Set up later js invocations to run with the breakpad injector loaded.
    # Originally, I intended for this to be used with LD_PRELOAD, but when
    # cross-compiling from 64- to 32-bit, that will fail and produce stderr
    # output when running any 64-bit commands, which breaks eg mozconfig
    # processing. So use the --dll command line mechanism universally.
    for v in ("JSTESTS_EXTRA_ARGS", "JITTEST_EXTRA_ARGS"):
        env[v] = "--args='--dll %s' %s" % (injector_lib, env.get(v, ""))

# Always run all enabled tests, even if earlier ones failed. But return the
# first failed status.
results = [("(make-nonempty)", 0)]

if "checks" in test_suites:
    results.append(("make check", run_test_command([MAKE, "check"])))

if "jittest" in test_suites:
    results.append(("make check-jit-test", run_test_command([MAKE, "check-jit-test"])))
if "jsapitests" in test_suites:
    jsapi_test_binary = os.path.join(OBJDIR, "dist", "bin", "jsapi-tests")
    test_env = env.copy()
    test_env["TOPSRCDIR"] = DIR.source
    if use_minidump and platform.system() == "Linux":
        test_env["LD_PRELOAD"] = injector_lib
    st = run_test_command([jsapi_test_binary], env=test_env)
    if st < 0:
        print("PROCESS-CRASH | jsapi-tests | application crashed")
        print("Return code: {}".format(st))
    results.append(("jsapi-tests", st))
if "jstests" in test_suites:
    results.append(("jstests", run_test_command([MAKE, "check-jstests"])))
if "gdb" in test_suites:
    test_script = os.path.join(DIR.js_src, "gdb", "run-tests.py")
    auto_args = ["-s", "-o", "--no-progress"] if AUTOMATION else []
    extra_args = env.get("GDBTEST_EXTRA_ARGS", "").split(" ")
    results.append(
        (
            "gdb",
            run_test_command([PYTHON, test_script, *auto_args, *extra_args, OBJDIR]),
        )
    )

# FIXME bug 1291449: This would be unnecessary if we could run msan with -mllvm
# -msan-keep-going, but in clang 3.8 it causes a hang during compilation.
if variant.get("ignore-test-failures"):
    logging.warning("Ignoring test results %s" % (results,))
    results = [("ignored", 0)]

if args.variant == "msan":
    files = filter(lambda f: f.startswith("sanitize_log."), os.listdir(OUTDIR))
    fullfiles = [os.path.join(OUTDIR, f) for f in files]

    # Summarize results
    sites = Counter()
    errors = Counter()
    for filename in fullfiles:
        with open(os.path.join(OUTDIR, filename), "rb") as fh:
            for line in fh:
                m = re.match(
                    r"^SUMMARY: \w+Sanitizer: (?:data race|use-of-uninitialized-value) (.*)",  # NOQA: E501
                    line.strip(),
                )
                if m:
                    # Some reports include file:line:column, some just
                    # file:line. Just in case it's nondeterministic, we will
                    # canonicalize to just the line number.
                    site = re.sub(r"^(\S+?:\d+)(:\d+)* ", r"\1 ", m.group(1))
                    sites[site] += 1

    # Write a summary file and display it to stdout.
    summary_filename = os.path.join(
        env["MOZ_UPLOAD_DIR"], "%s_summary.txt" % args.variant
    )
    with open(summary_filename, "wb") as outfh:
        for location, count in sites.most_common():
            print >> outfh, "%d %s" % (count, location)
    print(open(summary_filename, "rb").read())

    if "max-errors" in variant:
        max_allowed = variant["max-errors"]
        print("Found %d errors out of %d allowed" % (len(sites), max_allowed))
        if len(sites) > max_allowed:
            results.append(("too many msan errors", 1))

    # Gather individual results into a tarball. Note that these are
    # distinguished only by pid of the JS process running within each test, so
    # given the 16-bit limitation of pids, it's totally possible that some of
    # these files will be lost due to being overwritten.
    command = [
        "tar",
        "-C",
        OUTDIR,
        "-zcf",
        os.path.join(env["MOZ_UPLOAD_DIR"], "%s.tar.gz" % args.variant),
    ]
    command += files
    subprocess.call(command)

# Generate stacks from minidumps.
if use_minidump:
    venv_python = os.path.join(OBJDIR, "_virtualenvs", "common", "bin", "python3")
    run_command(
        [
            venv_python,
            os.path.join(DIR.source, "testing/mozbase/mozcrash/mozcrash/mozcrash.py"),
            os.getenv("TMPDIR", "/tmp"),
            os.path.join(OBJDIR, "dist/crashreporter-symbols"),
        ]
    )

for name, st in results:
    print("exit status %d for '%s'" % (st, name))

# Pick the "worst" exit status. SIGSEGV might give a status of -11, so use the
# maximum absolute value instead of just the maximum.
exit_status = max((st for _, st in results), key=abs)

# The exit status on Windows can be something like 2147483651 (0x80000003),
# which will be converted to status zero in the caller. Mask off the high bits,
# but if the result is zero then fall back to returning 1.
if exit_status & 0xFF:
    sys.exit(exit_status & 0xFF)
else:
    sys.exit(1 if exit_status else 0)
