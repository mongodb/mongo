import os
import shutil
import subprocess
import sys
import time

sys.path.append(".")

from buildscripts.engflow_auth import setup_auth
from buildscripts.install_bazel import install_bazel

# do not print to stdout as that is used for arg modifications
orig_stdout = sys.stdout
sys.stdout = sys.stderr

if (
    os.environ.get("MONGO_BAZEL_WRAPPER_DEBUG") == "1"
    and os.environ.get("MONGO_AUTOCOMPLETE_QUERY") != "1"
):

    def wrapper_debug(x):
        print("[WRAPPER_HOOK_DEBUG]: " + x, file=sys.stderr)
else:

    def wrapper_debug(x):
        pass


class BinAndSourceIncompatible(Exception):
    pass


class DuplicateSourceNames(Exception):
    pass


wrapper_debug(f"wrapper hook script is using {sys.executable}")


def get_buildozer_output(autocomplete_query):
    buildozer = shutil.which("buildozer")
    if not buildozer:
        buildozer = os.path.expanduser("~/.local/bin/buildozer")
        if not os.path.exists(buildozer):
            bazel_bin_dir = os.path.expanduser("~/.local/bin")
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


def engflow_auth(args):
    start = time.time()
    args_str = " ".join(args)
    if (
        "--config=local" not in args_str
        and "--config=public-release" not in args_str
        and "--config local" not in args_str
        and "--config public-release" not in args_str
    ):
        if os.environ.get("CI") is None:
            setup_auth(verbose=False)
    wrapper_debug(f"engflow auth time: {time.time() - start}")


def test_runner_interface(args, autocomplete_query, get_buildozer_output=get_buildozer_output):
    start = time.time()

    plus_autocomplete_query = False
    if autocomplete_query:
        str_args = " ".join(args)
        if "'//:*'" in str_args or "':*'" in str_args or "//:all" in str_args or ":all" in str_args:
            plus_autocomplete_query = True

    plus_starts = ("+", ":+", "//:+")
    skip_plus_interface = True
    for arg in args:
        if arg.startswith(plus_starts):
            skip_plus_interface = False

    if skip_plus_interface and not autocomplete_query:
        return args[1:]

    sources_to_bin = {}
    select_sources = {}
    current_select = None
    in_select = False
    c_exts = (".c", ".cc", ".cpp")

    def add_source_test(source_file, bin_file, sources_to_bin):
        src_key = os.path.splitext(
            os.path.basename(source_file.replace("//", "").replace(":", "/"))
        )[0]
        if src_key in sources_to_bin:
            raise DuplicateSourceNames(
                f"Two test files with the same name:\n  {bin_file}->{src_key}\n  {sources_to_bin[src_key]}->{src_key}"
            )
        if src_key == os.path.basename(bin_file.replace("//", "").replace(":", "/")):
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
            "//:+" + os.path.basename(test.replace("//", "").replace(":", "/"))
            for test in set(sources_to_bin.values())
        ]
        with open("/tmp/mongo_autocomplete_plus_targets", "w") as f:
            f.write(" ".join(autocomplete_target))
    elif autocomplete_query:
        with open("/tmp/mongo_autocomplete_plus_targets", "w") as f:
            f.write("")

    if autocomplete_query or plus_autocomplete_query:
        return args[1:]

    replacements = {}
    fileNameFilter = []
    bin_targets = []
    source_targets = {}

    for arg in args[1:]:
        if arg.startswith(plus_starts):
            test_name = arg[arg.find("+") + 1 :]
            real_target = sources_to_bin.get(test_name)

            if not real_target:
                for bin_target in set(sources_to_bin.values()):
                    if (
                        os.path.basename(bin_target.replace("//", "").replace(":", "/"))
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
                    os.path.basename(bin_target.replace("//", "").replace(":", "/"))
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
        new_args.append("--test_arg=-fileNameFilter")
        new_args.append(f"--test_arg={'|'.join(fileNameFilter)}")

    wrapper_debug(f"plus interface time: {time.time() - start}")

    return new_args


engflow_auth(sys.argv)

args = test_runner_interface(
    sys.argv, autocomplete_query=os.environ.get("MONGO_AUTOCOMPLETE_QUERY") == "1"
)

print(" ".join(args), file=orig_stdout)
