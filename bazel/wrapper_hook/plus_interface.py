import os
import pathlib
import platform
import shutil
import subprocess
import sys
import time

REPO_ROOT = pathlib.Path(__file__).parent.parent.parent
sys.path.append(str(REPO_ROOT))

from bazel.wrapper_hook.compiledb import generate_compiledb
from bazel.wrapper_hook.lint import run_rules_lint
from bazel.wrapper_hook.wrapper_debug import wrapper_debug


class BinAndSourceIncompatible(Exception):
    pass


class DuplicateSourceNames(Exception):
    pass

def get_buildozer_output(autocomplete_query):
    from buildscripts.install_bazel import install_bazel

    buildozer_name = "buildozer" if not platform.system() == "Windows" else "buildozer.exe"
    buildozer = shutil.which(buildozer_name)
    if not buildozer:
        buildozer = str(pathlib.Path(f"~/.local/bin/{buildozer_name}").expanduser())
        if not os.path.exists(buildozer):
            bazel_bin_dir = str(pathlib.Path("~/.local/bin").expanduser())
            if not os.path.exists(bazel_bin_dir):
                os.makedirs(bazel_bin_dir)
            install_bazel(bazel_bin_dir)

    p = subprocess.run(
        [buildozer, "print label srcs", "//src/...:%mongo_cc_unit_test"],
        capture_output=True,
        text=True,
    )

    if not autocomplete_query and p.returncode != 0:
        print("buildozer test target query failed:")
        print(p.args)
        print(p.stdout)
        print(p.stderr)
        sys.exit(1)

    return p.stdout


def check_bazel_command_type(args):
    with open(pathlib.Path(__file__).parent / "bazel_commands.commands") as f:
        bazel_commands = [line.strip() for line in f.readlines()]

    for arg in args:
        if arg in bazel_commands:
            return arg


def test_runner_interface(
    args, autocomplete_query, get_buildozer_output=get_buildozer_output, enterprise=True
):
    start = time.time()

    plus_autocomplete_query = False
    plus_starts = ("+", ":+", "//:+")
    skip_plus_interface = True
    compiledb_target = False
    lint_target = False
    persistent_compdb = True
    compiledb_targets = ["//:compiledb", ":compiledb", "compiledb"]
    lint_targets = ["//:lint", ":lint", "lint"]
    sources_to_bin = {}
    select_sources = {}
    current_select = None
    in_select = False
    c_exts = (".c", ".cc", ".cpp")
    replacements = {}
    fileNameFilter = []
    bin_targets = []
    source_targets = {}

    current_bazel_command = check_bazel_command_type(args)

    if autocomplete_query:
        str_args = " ".join(args)
        if "'//:*'" in str_args or "':*'" in str_args or "//:all" in str_args or ":all" in str_args:
            plus_autocomplete_query = True

    if os.environ.get("CI") is not None:
        persistent_compdb = False

    for arg in args:
        if arg in compiledb_targets:
            compiledb_target = True
        if arg in lint_targets:
            lint_target = True
        if arg == "--intree_compdb":
            replacements[arg] = []
            persistent_compdb = False
            skip_plus_interface = False
        if arg.startswith(plus_starts):
            skip_plus_interface = False

    if compiledb_target:
        generate_compiledb(args[0], persistent_compdb, enterprise)

    if lint_target:
        for lint_arg in lint_targets:
            try:
                command_start_index = args.index(lint_arg) + 1
            except ValueError:
                pass
        run_rules_lint(args[0], args[command_start_index:])

        return ["run", "lint", "--", "ALL_PASSING"]

    if skip_plus_interface and not autocomplete_query:
        return args[1:]

    def add_source_test(source_file, bin_file, sources_to_bin):
        src_key = pathlib.Path(
            pathlib.Path(source_file.replace("//", "").replace(":", "/")).name
        ).stem
        if src_key in sources_to_bin:
            raise DuplicateSourceNames(
                f"Two test files with the same name:\n  {bin_file}->{src_key}\n  {sources_to_bin[src_key]}->{src_key}"
            )
        if src_key == pathlib.Path(bin_file.replace("//", "").replace(":", "/")).name:
            src_key = f"{src_key}-{src_key}"
        sources_to_bin[src_key] = bin_file

    # this naively gets all possible source file targets
    for line in get_buildozer_output(autocomplete_query).splitlines():
        # non select case
        if line.startswith("//") and line.endswith("]"):
            in_select = False
            current_select = None
            tokens = line.split("[")
            binfile = tokens[0].strip()
            srcs = tokens[1][:-1].split(" ")
            for src in srcs:
                if src.endswith(c_exts):
                    add_source_test(src, binfile, sources_to_bin)
        else:
            if not in_select:
                current_select = line.split(" ")[0]
                select_sources[current_select] = []
                in_select = True
            for token in line.split('"'):
                if token.strip().endswith(c_exts):
                    add_source_test(token.strip(), current_select, sources_to_bin)

    if plus_autocomplete_query:
        autocomplete_target = ["//:+" + test for test in sources_to_bin.keys()]
        autocomplete_target += [
            "//:+" + pathlib.Path(test.replace("//", "").replace(":", "/")).name
            for test in set(sources_to_bin.values())
        ]
        autocomplete_target += ["//:compiledb"]
        with open("/tmp/mongo_autocomplete_plus_targets", "w") as f:
            f.write(" ".join(autocomplete_target))
    elif autocomplete_query:
        with open("/tmp/mongo_autocomplete_plus_targets", "w") as f:
            f.write("")

    if autocomplete_query or plus_autocomplete_query:
        return args[1:]

    for arg in args[1:]:
        if arg.startswith(plus_starts):
            test_name = arg[arg.find("+") + 1 :]
            real_target = sources_to_bin.get(test_name)

            if not real_target:
                for bin_target in set(sources_to_bin.values()):
                    if (
                        pathlib.Path(bin_target.replace("//", "").replace(":", "/")).name
                        == test_name
                    ):
                        bin_targets.append(bin_target)
                        real_target = bin_target
                replacements[arg] = [real_target]
            else:
                # defer source targets to see if we can skip redundant tests
                source_targets[test_name] = [arg, real_target]

    source_targets_without_bin_targets = []
    bins_from_source_added = []
    for test_name, values in source_targets.items():
        arg, real_target = values

        if real_target not in bin_targets:
            if real_target not in bins_from_source_added:
                replacements[arg] = [real_target]
                bins_from_source_added.append(real_target)
            else:
                replacements[arg] = []
            if test_name not in fileNameFilter:
                fileNameFilter += [test_name]
            source_targets_without_bin_targets.append(test_name)
        else:
            replacements[arg] = []

    if bin_targets and source_targets_without_bin_targets:
        raise BinAndSourceIncompatible(
            "Cannot mix source file test targets with different test binary targets.\n"
            + "Conflicting source targets:\n    "
            + "\n    ".join(source_targets_without_bin_targets)
            + "\n"
            + "Conflicting binary targets:\n    "
            + "\n    ".join(
                [
                    pathlib.Path(bin_target.replace("//", "").replace(":", "/")).name
                    for bin_target in bin_targets
                ]
            )
        )

    new_args = []
    replaced_already = []
    for arg in args[1:]:
        replaced = False
        for k, v in replacements.items():
            if v and v[0] is None:
                pass
            elif arg == k:
                if k not in replaced_already:
                    new_args.extend(v)
                    replaced_already.append(k)
                replaced = True
                break
        if not replaced:
            new_args.append(arg)

    if fileNameFilter:
        if current_bazel_command == "test":
            new_args.append("--test_arg=--fileNameFilter")
            new_args.append(f"--test_arg={'|'.join(fileNameFilter)}")
        if current_bazel_command == "run":
            if "--" not in args:
                new_args.append("--")
            new_args.append("--fileNameFilter")
            new_args.append(f"{'|'.join(fileNameFilter)}")

    wrapper_debug(f"plus interface time: {time.time() - start}")

    return new_args
