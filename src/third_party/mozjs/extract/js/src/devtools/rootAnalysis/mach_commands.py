# -*- coding: utf-8 -*-

# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.


import argparse
import html
import json
import logging
import os
import re
import textwrap
import webbrowser

# Command files like this are listed in build/mach_initialize.py in alphabetical
# order, but we need to access commands earlier in the sorted order to grab
# their arguments. Force them to load now.
import mozbuild.artifact_commands  # NOQA: F401
import mozbuild.build_commands  # NOQA: F401
import mozhttpd
from mach.base import FailedCommandError, MachError
from mach.decorators import Command, CommandArgument, SubCommand
from mach.registrar import Registrar
from mozbuild.base import BuildEnvironmentNotFoundException
from mozbuild.mozconfig import MozconfigLoader


# Use a decorator to copy command arguments off of the named command. Instead
# of a decorator, this could be straight code that edits eg
# MachCommands.build_shell._mach_command.arguments, but that looked uglier.
def inherit_command_args(command, subcommand=None):
    """Decorator for inheriting all command-line arguments from `mach build`.

    This should come earlier in the source file than @Command or @SubCommand,
    because it relies on that decorator having run first."""

    def inherited(func):
        handler = Registrar.command_handlers.get(command)
        if handler is not None and subcommand is not None:
            handler = handler.subcommand_handlers.get(subcommand)
        if handler is None:
            raise MachError(
                "{} command unknown or not yet loaded".format(
                    command if subcommand is None else command + " " + subcommand
                )
            )
        func._mach_command.arguments.extend(handler.arguments)
        return func

    return inherited


def state_dir():
    return os.environ.get("MOZBUILD_STATE_PATH", os.path.expanduser("~/.mozbuild"))


def tools_dir():
    if os.environ.get("MOZ_FETCHES_DIR"):
        # In automation, tools are provided by toolchain dependencies.
        return os.path.join(os.environ["HOME"], os.environ["MOZ_FETCHES_DIR"])

    # In development, `mach hazard bootstrap` installs the tools separately
    # to avoid colliding with the "main" compiler versions, which can
    # change separately (and the precompiled sixgill and compiler version
    # must match exactly).
    return os.path.join(state_dir(), "hazard-tools")


def sixgill_dir():
    return os.path.join(tools_dir(), "sixgill")


def gcc_dir():
    return os.path.join(tools_dir(), "gcc")


def script_dir(command_context):
    return os.path.join(command_context.topsrcdir, "js/src/devtools/rootAnalysis")


def get_work_dir(command_context, project, given):
    if given is not None:
        return given
    return os.path.join(command_context.topsrcdir, "haz-" + project)


def get_objdir(command_context, kwargs):
    project = kwargs["project"]
    objdir = kwargs["haz_objdir"]
    if objdir is None:
        objdir = os.environ.get("HAZ_OBJDIR")
    if objdir is None:
        objdir = os.path.join(command_context.topsrcdir, "obj-analyzed-" + project)
    return objdir


def ensure_dir_exists(dir):
    os.makedirs(dir, exist_ok=True)
    return dir


# Force the use of hazard-compatible installs of tools.
def setup_env_for_tools(env):
    gccbin = os.path.join(gcc_dir(), "bin")
    env["CC"] = os.path.join(gccbin, "gcc")
    env["CXX"] = os.path.join(gccbin, "g++")
    env["PATH"] = "{sixgill_dir}/usr/bin:{gccbin}:{PATH}".format(
        sixgill_dir=sixgill_dir(), gccbin=gccbin, PATH=env["PATH"]
    )


def setup_env_for_shell(env, shell):
    """Add JS shell directory to dynamic lib search path"""
    for var in ("LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH"):
        env[var] = ":".join(p for p in (env.get(var), os.path.dirname(shell)) if p)


@Command(
    "hazards",
    category="build",
    order="declaration",
    description="Commands for running the static analysis for GC rooting hazards",
)
def hazards(command_context):
    """Commands related to performing the GC rooting hazard analysis"""
    print("See `mach hazards --help` for a list of subcommands")


@inherit_command_args("artifact", "toolchain")
@SubCommand(
    "hazards",
    "bootstrap",
    description="Install prerequisites for the hazard analysis",
)
def bootstrap(command_context, **kwargs):
    orig_dir = os.getcwd()
    os.chdir(ensure_dir_exists(tools_dir()))
    try:
        kwargs["from_build"] = ("linux64-gcc-sixgill", "linux64-gcc-9")
        command_context._mach_context.commands.dispatch(
            "artifact", command_context._mach_context, subcommand="toolchain", **kwargs
        )
    finally:
        os.chdir(orig_dir)


CLOBBER_CHOICES = {"objdir", "work", "shell", "all"}


@SubCommand("hazards", "clobber", description="Clean up hazard-related files")
@CommandArgument("--project", default="browser", help="Build the given project.")
@CommandArgument("--application", dest="project", help="Build the given project.")
@CommandArgument("--haz-objdir", default=None, help="Hazard analysis objdir.")
@CommandArgument(
    "--work-dir", default=None, help="Directory for output and working files."
)
@CommandArgument(
    "what",
    default=["objdir", "work"],
    nargs="*",
    help="Target to clobber, must be one of {{{}}} (default "
    "objdir and work).".format(", ".join(CLOBBER_CHOICES)),
)
def clobber(command_context, what, **kwargs):
    from mozbuild.controller.clobber import Clobberer

    what = set(what)
    if "all" in what:
        what.update(CLOBBER_CHOICES)
    invalid = what - CLOBBER_CHOICES
    if invalid:
        print(
            "Unknown clobber target(s): {}. Choose from {{{}}}".format(
                ", ".join(invalid), ", ".join(CLOBBER_CHOICES)
            )
        )
        return 1

    try:
        substs = command_context.substs
    except BuildEnvironmentNotFoundException:
        substs = {}

    if "objdir" in what:
        objdir = get_objdir(command_context, kwargs)
        print(f"removing {objdir}")
        Clobberer(command_context.topsrcdir, objdir, substs).remove_objdir(full=True)
    if "work" in what:
        project = kwargs["project"]
        work_dir = get_work_dir(command_context, project, kwargs["work_dir"])
        print(f"removing {work_dir}")
        Clobberer(command_context.topsrcdir, work_dir, substs).remove_objdir(full=True)
    if "shell" in what:
        objdir = os.path.join(command_context.topsrcdir, "obj-haz-shell")
        print(f"removing {objdir}")
        Clobberer(command_context.topsrcdir, objdir, substs).remove_objdir(full=True)


@inherit_command_args("build")
@SubCommand(
    "hazards", "build-shell", description="Build a shell for the hazard analysis"
)
@CommandArgument(
    "--mozconfig",
    default=None,
    metavar="FILENAME",
    help="Build with the given mozconfig.",
)
def build_shell(command_context, **kwargs):
    """Build a JS shell to use to run the rooting hazard analysis."""
    # The JS shell requires some specific configuration settings to execute
    # the hazard analysis code, and configuration is done via mozconfig.
    # Subprocesses find MOZCONFIG in the environment, so we can't just
    # modify the settings in this process's loaded version. Pass it through
    # the environment.

    default_mozconfig = "js/src/devtools/rootAnalysis/mozconfig.haz_shell"
    mozconfig_path = (
        kwargs.pop("mozconfig", None)
        or os.environ.get("MOZCONFIG")
        or default_mozconfig
    )
    mozconfig_path = os.path.join(command_context.topsrcdir, mozconfig_path)
    loader = MozconfigLoader(command_context.topsrcdir)
    mozconfig = loader.read_mozconfig(mozconfig_path)

    # Validate the mozconfig settings in case the user overrode the default.
    configure_args = mozconfig["configure_args"]
    if "--enable-ctypes" not in configure_args:
        raise FailedCommandError(
            "ctypes required in hazard JS shell, mozconfig=" + mozconfig_path
        )

    # Transmit the mozconfig location to build subprocesses.
    os.environ["MOZCONFIG"] = mozconfig_path

    setup_env_for_tools(os.environ)

    # Set a default objdir for the shell, for developer builds.
    os.environ.setdefault(
        "MOZ_OBJDIR", os.path.join(command_context.topsrcdir, "obj-haz-shell")
    )

    return command_context._mach_context.commands.dispatch(
        "build", command_context._mach_context, **kwargs
    )


def read_json_file(filename):
    with open(filename) as fh:
        return json.load(fh)


def ensure_shell(command_context, objdir):
    if objdir is None:
        objdir = os.path.join(command_context.topsrcdir, "obj-haz-shell")

    try:
        binaries = read_json_file(os.path.join(objdir, "binaries.json"))
        info = [b for b in binaries["programs"] if b["program"] == "js"][0]
        return os.path.join(objdir, info["install_target"], "js")
    except (OSError, KeyError):
        raise FailedCommandError(
            """\
no shell found in %s -- must build the JS shell with `mach hazards build-shell` first"""
            % objdir
        )


def validate_mozconfig(command_context, kwargs):
    app = kwargs.pop("project")
    default_mozconfig = "js/src/devtools/rootAnalysis/mozconfig.%s" % app
    mozconfig_path = (
        kwargs.pop("mozconfig", None)
        or os.environ.get("MOZCONFIG")
        or default_mozconfig
    )
    mozconfig_path = os.path.join(command_context.topsrcdir, mozconfig_path)

    loader = MozconfigLoader(command_context.topsrcdir)
    mozconfig = loader.read_mozconfig(mozconfig_path)
    configure_args = mozconfig["configure_args"]

    # Require an explicit --enable-project/application=APP (even if you just
    # want to build the default browser project.)
    if (
        "--enable-project=%s" % app not in configure_args
        and "--enable-application=%s" % app not in configure_args
    ):
        raise FailedCommandError(
            textwrap.dedent(
                f"""\
            mozconfig {mozconfig_path} builds wrong project.
            unset MOZCONFIG to use the default {default_mozconfig}\
            """
            )
        )

    if not any("--with-compiler-wrapper" in a for a in configure_args):
        raise FailedCommandError(
            "mozconfig must wrap compiles with --with-compiler-wrapper"
        )

    return mozconfig_path


@inherit_command_args("build")
@SubCommand(
    "hazards",
    "gather",
    description="Gather analysis data by compiling the given project",
)
@CommandArgument("--project", default="browser", help="Build the given project.")
@CommandArgument("--application", dest="project", help="Build the given project.")
@CommandArgument(
    "--haz-objdir", default=None, help="Write object files to this directory."
)
@CommandArgument(
    "--work-dir", default=None, help="Directory for output and working files."
)
def gather_hazard_data(command_context, **kwargs):
    """Gather analysis information by compiling the tree"""
    project = kwargs["project"]
    objdir = get_objdir(command_context, kwargs)

    work_dir = get_work_dir(command_context, project, kwargs["work_dir"])
    ensure_dir_exists(work_dir)
    with open(os.path.join(work_dir, "defaults.py"), "wt") as fh:
        data = textwrap.dedent(
            """\
            analysis_scriptdir = "{script_dir}"
            objdir = "{objdir}"
            source = "{srcdir}"
            sixgill = "{sixgill_dir}/usr/libexec/sixgill"
            sixgill_bin = "{sixgill_dir}/usr/bin"
        """
        ).format(
            script_dir=script_dir(command_context),
            objdir=objdir,
            srcdir=command_context.topsrcdir,
            sixgill_dir=sixgill_dir(),
            gcc_dir=gcc_dir(),
        )
        fh.write(data)

    buildscript = " ".join(
        [
            command_context.topsrcdir + "/mach hazards compile",
            *kwargs.get("what", []),
            "--job-size=3.0",  # Conservatively estimate 3GB/process
            "--project=" + project,
            "--haz-objdir=" + objdir,
        ]
    )
    args = [
        os.path.join(script_dir(command_context), "run_complete"),
        "--foreground",
        "--no-logs",
        "--build-root=" + objdir,
        "--wrap-dir=" + sixgill_dir() + "/usr/libexec/sixgill/scripts/wrap_gcc",
        "--work-dir=work",
        "-b",
        sixgill_dir() + "/usr/bin",
        "--buildcommand=" + buildscript,
        ".",
    ]

    return command_context.run_process(args=args, cwd=work_dir, pass_thru=True)


@inherit_command_args("build")
@SubCommand("hazards", "compile", description=argparse.SUPPRESS)
@CommandArgument(
    "--mozconfig",
    default=None,
    metavar="FILENAME",
    help="Build with the given mozconfig.",
)
@CommandArgument("--project", default="browser", help="Build the given project.")
@CommandArgument("--application", dest="project", help="Build the given project.")
@CommandArgument(
    "--haz-objdir",
    default=os.environ.get("HAZ_OBJDIR"),
    help="Write object files to this directory.",
)
def inner_compile(command_context, **kwargs):
    """Build a source tree and gather analysis information while running
    under the influence of the analysis collection server."""

    env = os.environ

    # Check whether we are running underneath the manager (and therefore
    # have a server to talk to).
    if "XGILL_CONFIG" not in env:
        raise FailedCommandError(
            "no sixgill manager detected. `mach hazards compile` "
            + "should only be run from `mach hazards gather`"
        )

    mozconfig_path = validate_mozconfig(command_context, kwargs)

    # Communicate mozconfig to build subprocesses.
    env["MOZCONFIG"] = os.path.join(command_context.topsrcdir, mozconfig_path)

    # hazard mozconfigs need to find binaries in .mozbuild
    env["MOZBUILD_STATE_PATH"] = state_dir()

    # Suppress the gathering of sources, to save disk space and memory.
    env["XGILL_NO_SOURCE"] = "1"

    setup_env_for_tools(env)

    if "haz_objdir" in kwargs:
        env["MOZ_OBJDIR"] = kwargs.pop("haz_objdir")

    return command_context._mach_context.commands.dispatch(
        "build", command_context._mach_context, **kwargs
    )


@SubCommand(
    "hazards", "analyze", description="Analyzed gathered data for rooting hazards"
)
@CommandArgument(
    "--project",
    default="browser",
    help="Analyze the output for the given project.",
)
@CommandArgument("--application", dest="project", help="Build the given project.")
@CommandArgument(
    "--shell-objdir",
    default=None,
    help="objdir containing the optimized JS shell for running the analysis.",
)
@CommandArgument(
    "--work-dir", default=None, help="Directory for output and working files."
)
@CommandArgument(
    "--jobs", "-j", default=None, type=int, help="Number of parallel analyzers."
)
@CommandArgument(
    "--verbose",
    "-v",
    default=False,
    action="store_true",
    help="Display executed commands.",
)
@CommandArgument(
    "--from-stage",
    default=None,
    help="Stage to begin running at ('list' to see all).",
)
@CommandArgument(
    "extra",
    nargs=argparse.REMAINDER,
    default=(),
    help="Remaining non-optional arguments to analyze.py script",
)
def analyze(
    command_context,
    project,
    shell_objdir,
    work_dir,
    jobs,
    verbose,
    from_stage,
    extra,
):
    """Analyzed gathered data for rooting hazards"""

    shell = ensure_shell(command_context, shell_objdir)
    args = [
        os.path.join(script_dir(command_context), "analyze.py"),
        "--js",
        shell,
        *extra,
    ]

    if from_stage is None:
        pass
    elif from_stage == "list":
        args.append("--list")
    else:
        args.extend(["--first", from_stage])

    if jobs is not None:
        args.extend(["-j", jobs])

    if verbose:
        args.append("-v")

    setup_env_for_tools(os.environ)
    setup_env_for_shell(os.environ, shell)

    work_dir = get_work_dir(command_context, project, work_dir)
    return command_context.run_process(args=args, cwd=work_dir, pass_thru=True)


@SubCommand(
    "hazards",
    "self-test",
    description="Run a self-test to verify hazards are detected",
)
@CommandArgument(
    "--shell-objdir",
    default=None,
    help="objdir containing the optimized JS shell for running the analysis.",
)
@CommandArgument(
    "extra",
    nargs=argparse.REMAINDER,
    help="Remaining non-optional arguments to pass to run-test.py",
)
def self_test(command_context, shell_objdir, extra):
    """Analyzed gathered data for rooting hazards"""
    shell = ensure_shell(command_context, shell_objdir)
    args = [
        os.path.join(script_dir(command_context), "run-test.py"),
        "-v",
        "--js",
        shell,
        "--sixgill",
        os.path.join(tools_dir(), "sixgill"),
        "--gccdir",
        gcc_dir(),
    ]
    args.extend(extra)

    setup_env_for_tools(os.environ)
    setup_env_for_shell(os.environ, shell)

    return command_context.run_process(args=args, pass_thru=True)


def annotated_source(filename, query):
    """The index page has URLs of the format <http://.../path/to/source.cpp?L=m-n#m>.
    The `#m` part will be stripped off and used by the browser to jump to the correct line.
    The `?L=m-n` or `?L=m` parameter will be processed here on the server to highlight
    the given line range."""
    linequery = query.replace("L=", "")
    if "-" in linequery:
        line0, line1 = linequery.split("-", 1)
    else:
        line0, line1 = linequery or "0", linequery or "0"
    line0 = int(line0)
    line1 = int(line1)

    fh = open(filename, "rt")

    out = "<pre>"
    for lineno, line in enumerate(fh, 1):
        processed = f"{lineno} <span id='{lineno}'"
        if line0 <= lineno and lineno <= line1:
            processed += " style='background: yellow'"
        processed += ">" + html.escape(line.rstrip()) + "</span>\n"
        out += processed

    return out


@SubCommand(
    "hazards", "view", description="Display a web page describing any hazards found"
)
@CommandArgument(
    "--project",
    default="browser",
    help="Analyze the output for the given project.",
)
@CommandArgument("--application", dest="project", help="Build the given project.")
@CommandArgument(
    "--haz-objdir", default=None, help="Write object files to this directory."
)
@CommandArgument(
    "--work-dir", default=None, help="Directory for output and working files."
)
@CommandArgument("--port", default=6006, help="Port of the web server")
@CommandArgument(
    "--serve-only",
    default=False,
    action="store_true",
    help="Serve only, do not navigate to page",
)
def view_hazards(command_context, project, haz_objdir, work_dir, port, serve_only):
    work_dir = get_work_dir(command_context, project, work_dir)
    haztop = os.path.basename(work_dir)
    if haz_objdir is None:
        haz_objdir = os.environ.get("HAZ_OBJDIR")
    if haz_objdir is None:
        haz_objdir = os.path.join(command_context.topsrcdir, "obj-analyzed-" + project)

    httpd = None

    def serve_source_file(request, path):
        info = {"req": path}

        def log(fmt, level=logging.INFO):
            return command_context.log(level, "view-hazards", info, fmt)

        if path in ("", f"{haztop}"):
            info["dest"] = f"/{haztop}/hazards.html"
            info["code"] = 301
            log("serve '{req}' -> {code} {dest}")
            return (info["code"], {"Location": info["dest"]}, "")

        # Allow files to be served from the source directory or the objdir.
        roots = (command_context.topsrcdir, haz_objdir)

        try:
            # Validate the path. Some source files have weird characters in their paths (eg "+"), but they
            # all start with an alphanumeric or underscore.
            command_context.log(
                logging.DEBUG, "view-hazards", {"path": path}, "Raw path: {path}"
            )
            path_component = r"\w[\w\-\.\+]*"
            if not re.match(f"({path_component}/)*{path_component}$", path):
                raise ValueError("invalid path")

            # Resolve the path to under one of the roots, and
            # ensure that the actual file really is underneath a root directory.
            for rootdir in roots:
                fullpath = os.path.join(rootdir, path)
                info["path"] = fullpath
                fullpath = os.path.realpath(fullpath)
                if os.path.isfile(fullpath):
                    # symlinks between roots are ok, but not symlinks outside of the roots.
                    tops = [
                        d
                        for d in roots
                        if fullpath.startswith(os.path.realpath(d) + "/")
                    ]
                    if len(tops) > 0:
                        break  # Found a file underneath a root.
            else:
                raise IOError("not found")

            html = annotated_source(fullpath, request.query)
            log("serve '{req}' -> 200 {path}")
            return (
                200,
                {"Content-type": "text/html", "Content-length": len(html)},
                html,
            )
        except (IOError, ValueError):
            log("serve '{req}' -> 404 {path}", logging.ERROR)
            return (
                404,
                {"Content-type": "text/plain"},
                "We don't have that around here. Don't be asking for it.",
            )

    httpd = mozhttpd.MozHttpd(
        port=port,
        docroot=None,
        path_mappings={"/" + haztop: work_dir},
        urlhandlers=[
            # Treat everything not starting with /haz-browser/ (or /haz-js/)
            # as a source file to be processed. Everything else is served
            # as a plain file.
            {
                "method": "GET",
                "path": "/(?!haz-" + project + "/)(.*)",
                "function": serve_source_file,
            },
        ],
        log_requests=True,
    )

    # The mozhttpd request handler class eats log messages.
    httpd.handler_class.log_message = lambda self, format, *args: command_context.log(
        logging.INFO, "view-hazards", {}, format % args
    )

    print("Serving at %s:%s" % (httpd.host, httpd.port))

    httpd.start(block=False)
    url = httpd.get_url(f"/{haztop}/hazards.html")
    display_url = True
    if not serve_only:
        try:
            webbrowser.get().open_new_tab(url)
            display_url = False
        except Exception:
            pass
    if display_url:
        print("Please open %s in a browser." % url)

    print("Hit CTRL+c to stop server.")
    httpd.server.join()
