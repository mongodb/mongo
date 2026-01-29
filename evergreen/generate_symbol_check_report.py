import json
import os
import sys

from buildscripts.simple_report import make_report, put_report, try_combine_reports
from buildscripts.util.read_config import read_config_file


def _read_optional_text_file(path: str) -> str | None:
    try:
        with open(path) as f:
            content = f.read().strip()
    except OSError:
        return None
    return content or None


def _with_debug_target(target: str) -> str:
    """Symbol-check aspect emits cc_library-like targets as *_with_debug."""
    if target.endswith("_with_debug"):
        return target
    return f"{target}_with_debug"


# 1. detect if we should run symbol-check reporting
expansions = read_config_file("../expansions.yml")
symbol_check = expansions.get("run_for_symbol_check", None)

if not symbol_check:
    sys.exit(0)

failures = []

# Prefer the exact Bazel flags/invocation emitted by the Evergreen Bazel compile script.
# This script is run from the "src" directory (see evergreen/run_python_script.sh).
bazel_build_flags = _read_optional_text_file(".bazel_build_flags")
bazel_build_invocation = _read_optional_text_file(".bazel_build_invocation")

# 2. walk bazel-bin for *_checked files emitted by the aspect
for root, _, files in os.walk("bazel-bin"):
    for name in files:
        if not name.endswith("_checked"):
            continue

        checked_path = os.path.join(root, name)
        # default values in case we fall back to text mode
        target = None
        sym_file = None
        missing = None
        status = None

        with open(checked_path) as f:
            data = json.load(f)

        status = data.get("status")
        target = data.get("target")
        sym_file = data.get("sym_file")
        missing = data.get("missing", [])

        if status == "failed":
            # build content for the report
            lines = []
            lines.append(f"Symbol check failed for: {target}")
            lines.append("Missing symbols:")
            for m in missing:
                lines.append(f"  - {m}")
            lines.append(
                f"Please check to see if {target} is missing any deps that would include the symbols above"
            )

            # reproduction hint – adjust this to your CI config name
            # if you have a real config, e.g. --config=symbol-check, use that
            repro_target = target or sym_file or checked_path
            lines.append("")
            lines.append("To reproduce:")
            repro_target = _with_debug_target(str(repro_target))
            if bazel_build_flags:
                lines.append(f"  bazel build {bazel_build_flags} {repro_target}")
            else:
                lines.append(f"  bazel build --config=symbol-checker {repro_target}")

            content = "\n".join(lines)

            # for symbol check we don't have a real src path like clang-tidy,
            # so use a synthetic "file" name that encodes the bazel target
            synthetic_file = f"symbol_check:{target or checked_path}"

            failures.append((synthetic_file, content))

# 3. write a helper invocation file
with open("bazel-invocation.txt", "w") as f:
    if bazel_build_invocation:
        # Keep the exact command that CI ran (targets included).
        f.write(bazel_build_invocation)
    elif bazel_build_flags:
        # Fall back to the exact flags from CI (best effort on targets).
        f.write(f"bazel build {bazel_build_flags} //src/...")
    else:
        # Last-resort fallback.
        f.write("bazel build --config=symbol-checker //src/...")


# 4. emit reports
if failures:
    for filename, content in failures:
        report = make_report(filename, content, 1)
        try_combine_reports(report)
        put_report(report)
    sys.exit(1)
else:
    report = make_report("symbol-check", "all symbol checks passed", 0)
    try_combine_reports(report)
    put_report(report)
    sys.exit(0)
