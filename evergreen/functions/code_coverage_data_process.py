import glob
import os
import platform
import subprocess
import sys

from buildscripts.util.expansions import get_expansion

# This script is used to gather code coverage data from the build.
# It is run as part of the Evergreen build process.
# It is not intended to be run directly.


def get_bazel_coverage_report_file() -> str:
    BAZEL_BINARY = "bazel"  # simplified since we already restrict which platforms can run coverage
    proc = subprocess.run([BAZEL_BINARY, "info", "output_path"], check=True, capture_output=True)
    bazel_output_location = proc.stdout.decode("utf-8").strip()
    bazel_coverage_report_location = os.path.join(
        bazel_output_location, "_coverage", "_coverage_report.dat"
    )
    return bazel_coverage_report_location


BAZEL_COVERAGE_SUMMARY_FILE = "bazel-coverage-summary.txt"


def _parse_lcov_line_coverage(report_path: str) -> dict[str, dict[int, int]]:
    """Parse an lcov tracefile into {source_file: {line_number: hit_count}}.

    A single source file can appear in more than one record when reports are merged, so hit
    counts are accumulated per line rather than overwritten. Only DA: (line) records are read;
    branch and function records are ignored.
    """
    per_file: dict[str, dict[int, int]] = {}
    current: dict[int, int] | None = None
    with open(report_path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if line.startswith("SF:"):
                current = per_file.setdefault(line[3:].strip(), {})
            elif line.startswith("end_of_record"):
                current = None
            elif line.startswith("DA:") and current is not None:
                # DA:<line>,<hits>[,<checksum>]
                fields = line[3:].strip().split(",")
                try:
                    line_no = int(fields[0])
                    hits = int(fields[1])
                except (IndexError, ValueError):
                    continue
                current[line_no] = current.get(line_no, 0) + hits
    return per_file


def _write_coverage_summary(report_path: str, summary_path: str) -> None:
    """Log summary coverage stats and write a per-file breakdown for upload as an artifact.

    The summary doubles as a diagnostic: a "Nothing to report" from Coveralls can be classified
    as a genuinely empty report vs. one whose SF: paths don't match the repo layout.
    """
    per_file = _parse_lcov_line_coverage(report_path)

    def covered_of(lines: dict[int, int]) -> int:
        return sum(1 for hits in lines.values() if hits > 0)

    total_executable = sum(len(lines) for lines in per_file.values())
    total_covered = sum(covered_of(lines) for lines in per_file.values())

    def percent(covered: int, executable: int) -> float:
        return 100.0 * covered / executable if executable else 0.0

    print(
        f"[coverage-summary] files={len(per_file)} executable_lines={total_executable} "
        f"covered_lines={total_covered} "
        f"coverage={percent(total_covered, total_executable):.0f}% "
        f"report={report_path} ({os.path.getsize(report_path)} bytes)"
    )

    with open(summary_path, "w", encoding="utf-8") as out:
        out.write(f"Coverage report: {report_path}\n")
        out.write(
            f"Files: {len(per_file)}  Executable lines: {total_executable}  "
            f"Covered lines: {total_covered}  "
            f"Coverage: {percent(total_covered, total_executable):.0f}%\n\n"
        )
        out.write(f"{'Lines':>8} {'Exec':>8} {'Uncovered':>10} {'Cover':>8}  File\n")
        for path, lines in sorted(per_file.items()):
            executable = len(lines)
            covered = covered_of(lines)
            out.write(
                f"{executable:>8} {covered:>8} {executable - covered:>10} "
                f"{percent(covered, executable):>7.0f}%  {path}\n"
            )
        out.write(
            f"\n{total_executable:>8} {total_covered:>8} "
            f"{total_executable - total_covered:>10} "
            f"{percent(total_covered, total_executable):>7.0f}%  TOTAL\n"
        )
    print(f"[coverage-summary] wrote per-file breakdown to {summary_path}")


def main():
    should_gather_code_coverage = get_expansion("gather_code_coverage_results", False)
    if not should_gather_code_coverage:
        print("Missing 'gather_code_coverage_results' expansion, skipping code coverage.")
        return 0

    gcov_binary = get_expansion("gcov_tool", None)
    if not gcov_binary:
        print(
            "Missing 'gcov_tool' expansion, skipping code coverage because this is likely not a code coverage variant."
        )
        return 0

    disallowed_arches = {"s390x", "s390", "ppc64le", "ppc64", "ppc", "ppcle"}
    arch = platform.uname().machine.lower()
    print(f"Detected arch: {arch}")
    if arch in disallowed_arches:
        raise RuntimeError(f"Code coverage not supported on architecture '{arch}'.")

    if not os.path.exists(".git"):
        raise RuntimeError(
            "No git repo found in working directory. Code coverage needs git repo to function."
        )

    bazel_coverage_report_location = get_bazel_coverage_report_file()
    if os.path.exists(bazel_coverage_report_location):
        print("Found bazel coverage report.")
        _write_coverage_summary(bazel_coverage_report_location, BAZEL_COVERAGE_SUMMARY_FILE)
        # no gcda files are generated from bazel coverage so we can exit early here
        return 0

    print(f"No bazel coverage report found at {bazel_coverage_report_location}")

    # because of bazel symlink shenanigans, the bazel gcda and gcno files are put in different
    # directories when the GCOV_PREFIX and GCOV_PREFIX_STRIP env vars are used. We manually
    # put the gcno files where the gcda files are generated to fix this.
    has_bazel_gcno = False
    workdir = get_expansion("workdir")
    bazel_output_dir = os.path.join(workdir, "bazel-out")
    for file in glob.iglob("./**/bazel-out/**/*.gcno", root_dir=workdir, recursive=True):
        has_bazel_gcno = True
        parts = file.split("bazel-out/")
        assert len(parts) == 2, "Something went wrong, path was not split into 2 parts."
        old_path = os.path.join(workdir, file)
        new_path = os.path.join(bazel_output_dir, parts[1])
        new_dir = os.path.dirname(new_path)
        os.makedirs(new_dir, exist_ok=True)
        os.rename(old_path, new_path)

    if not has_bazel_gcno:
        raise RuntimeError("Neither bazel coverage nor gcno files were found.")

    coveralls_report = "gcovr-coveralls.json"

    args = [
        "python3",
        "-m",
        "gcovr",
        "--output",
        coveralls_report,
        "--coveralls-pretty",
        "--txt",
        "gcovr-coveralls.txt",
        "--print-summary",
        "--exclude",
        "build/debug/.*",
        "--exclude",
        ".*bazel-out/.*",
        "--exclude",
        ".*external/.*mongo_toolchain.*",
        "--exclude",
        r".*src/.*_gen\.(h|hpp|cpp)",
        "--exclude",
        r".*src/mongo/db/cst/grammar\.yy",
        "--exclude",
        ".*src/mongo/idl/.*",
        "--exclude",
        r".*src/mongo/.*_test\.(h|hpp|cpp)",
        "--exclude",
        ".*src/mongo/dbtests/.*",
        "--exclude",
        ".*src/mongo/unittest/.*",
        "--exclude",
        ".*/third_party/.*",
        "--gcov-ignore-errors",
        "source_not_found",
        "--gcov-ignore-parse-errors",
        "negative_hits.warn",
        "--gcov-exclude-directories",
        ".*src/mongo/dbtests/.*",
        "--gcov-exclude-directories",
        ".*src/mongo/idl/.*",
        "--gcov-exclude-directories",
        ".*src/mongo/unittest/.*",
        "--gcov-exclude-directories",
        ".*/third_party/.*",
        "--gcov-executable",
        gcov_binary,
        bazel_output_dir,
    ]

    print("Running gcovr command")
    process = subprocess.run(
        args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, encoding="utf-8"
    )
    print(process.stdout)
    if process.returncode != 0:
        raise RuntimeError(f"gcovr failed with code: {process.returncode}")

    if not os.path.exists(coveralls_report):
        raise RuntimeError(f"Could not find coveralls json report at {coveralls_report}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
