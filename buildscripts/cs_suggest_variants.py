#!/usr/bin/env python3
"""Suggest Evergreen suites and variants for changed Cluster Scalability tests.

The normal path, run from a MongoDB worktree, is:

    python3 buildscripts/cs_suggest_variants.py

How it works:

1. Collect the changed files (committed against ``origin/master``, staged and
   unstaged) and turn them into targets: jstests by path, and C++ unit-test
   binaries by mapping each changed source through the ``mongo_cc_unit_test``
   rules that compile it. Production-only C++ changes are ignored, because
   there is no reliable source-file-to-suite mapping.
2. Load the guidance for those targets: the suites a test should run in, the
   variants it has failed on, and the measured per-variant failure rates.
   These come from the monthly analysis artifacts, cloned from the guidance
   repository, falling back to a local ``cs_bf_analysis`` directory.
3. Expand the Evergreen variant matrix for both the master and the nightly
   project, so a recommendation only names variants that really exist there.
4. Choose what to run. Each advised jstest suite chooses one primary variant,
   preferring one the test has actually failed on and skipping suites the
   required set already covers. Each C++ binary takes its variants straight
   from rate evidence that is both repeated and frequent enough to reproduce
   in a patch. The final Evergreen command submits the union of all selected
   variants and tasks. Evergreen may therefore run a selected task on every
   selected variant where that task exists. This broader cross-coverage is
   intentional and may schedule more work than the primary choices alone.
5. Print the plan, the evidence behind it, and the ``evergreen patch``
   command. Suites are reported as the guidance names them but requested as
   the tasks that run them, which is what ``-t`` matches. Evidence measured
   only on nightly-only variants becomes a second command against the nightly
   project. ``--run-evergreen`` submits them.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import re
import shlex
import subprocess
import sys
from contextlib import contextmanager
from dataclasses import dataclass, field
from functools import lru_cache
from pathlib import Path
from tempfile import TemporaryDirectory
from typing import Callable, Iterable, Iterator, Sequence

import yaml

REPO_ROOT = Path(__file__).resolve().parents[1]

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(str(REPO_ROOT))

from buildscripts.ciconfig.evergreen import Task, find_evergreen_binary, parse_evergreen_file

# Under `bazel run`, __file__ lives in the runfiles tree, so prefer the real
# checkout that Bazel exposes via BUILD_WORKSPACE_DIRECTORY.
WORKSPACE_ROOT = Path(os.environ.get("BUILD_WORKSPACE_DIRECTORY") or REPO_ROOT)

REPO = Path(os.environ.get("MONGODB_REPO") or WORKSPACE_ROOT)
GUIDANCE_REPO = os.environ.get("CS_BF_GUIDANCE_REPO", "git@github.com:10gen/employees.git")
GUIDANCE_REF = os.environ.get("CS_BF_GUIDANCE_REF", "master")
# Relative to the GUIDANCE_REPO checkout root, e.g. employees/home/abdul.qadeer/cs_bf_analysis.
GUIDANCE_SUBDIR = Path(
    os.environ.get("CS_BF_GUIDANCE_SUBDIR") or "home/abdul.qadeer/cs_bf_analysis"
)
PROJECT = "mongodb-mongo-master"
NIGHTLY_PROJECT = "mongodb-mongo-master-nightly"
# Each project evaluates its own root config, and the two variant sets are very
# nearly disjoint, so evidence from one cannot be patched against the other.
PROJECT_CONFIGS = {
    PROJECT: "etc/evergreen.yml",
    NIGHTLY_PROJECT: "etc/evergreen_nightly.yml",
}
BASE_REF = "origin/master"
SUITE_SHEET = "17_suite_guidance_all_cs_tests.csv"
VARIANT_SHEET = "16_variant_guidance.csv"
RATE_SUMMARY = "nd_failure_rates_summary.json"
CPP_MIN_FAILURE_RATE = 0.01
CPP_MIN_FAILURES = 2

JSTEST = "jstest"
CPP_UNIT_TEST = "cpp_unit_test"
CPP_SOURCE_SUFFIXES = (".cpp", ".cc", ".cxx")
CPP_HEADER_SUFFIXES = (".h", ".hh", ".hpp", ".hxx", ".inl")
CPP_FILE_SUFFIXES = CPP_SOURCE_SUFFIXES + CPP_HEADER_SUFFIXES
# The mongo_cc_unit_test attributes that name files the binary compiles.
# private_hdrs counts: a changed test fixture header reaches its binary only
# through it. The _select variants hold the configuration-gated entries.
SOURCE_ATTRIBUTES = (
    "srcs",
    "hdrs",
    "private_hdrs",
    "srcs_select",
    "hdrs_select",
    "private_hdrs_select",
)


class EvergreenUnavailable(Exception):
    """The evergreen CLI is not installed, so the config cannot be evaluated.

    This is the only condition that falls back to the component-file
    approximation. A config the CLI rejects is a real error: recommending from
    an approximate matrix would hide it behind a normal-looking report.
    """


class GuidanceValidationError(ValueError):
    """A cloned or fallback guidance artifact is present but incomplete."""


def _task_aliases(name: str) -> set[str]:
    """Return the names a task or suite can be matched by across the variant matrix.

    A generated task appears as ``foo_gen`` in the configuration but as ``foo``
    in the guidance, and either spelling can reach us, so both are matched.
    """
    return {name, name.removesuffix("_gen"), f"{name}_gen"}


@dataclass
class Target:
    """A changed test the recommendation is made for."""

    name: str
    kind: str


@dataclass
class TestGuidance:
    """The published advice for a single test."""

    basis: str
    suites: list[str]
    kind: str = ""
    sheet: str = ""
    variant_context: list[str] = field(default_factory=list)
    episodes: str = ""
    verified: str = ""
    behind: str = ""
    covered: str = ""


@dataclass
class RateEvidence:
    """Observed failures for one test in one variant/task cell."""

    variant: str
    task: str
    runs: int
    failures: int
    project: str = PROJECT

    @property
    def one_in_n(self) -> float | None:
        """Return the mean number of runs between failures, or None if it never failed."""
        return self.runs / self.failures if self.failures else None

    def summary(self) -> str:
        """Describe the cell in one line, naming the project when it is not the patch target."""
        where = f"{self.variant}/{self.task}"
        if self.project != PROJECT:
            where += f" (in {self.project}, not {PROJECT})"
        if self.one_in_n:
            return f"fails about 1 in {self.one_in_n:.0f} runs on {where}"
        return f"{self.failures} failures in {self.runs} runs on {where}"

    def why(self, subject: str) -> str:
        """Explain why this cell justifies adding its variant.

        The reason is stated as the cell selecting the variant, not as the
        variant being optional. A nightly-required variant is deliberately
        chosen here, because the nightly patch carries no alias to schedule
        it, so calling it "not required" would be false.
        """
        rate = f"~1 in {self.one_in_n:.0f}" if self.one_in_n else f"{self.failures / self.runs:.1%}"
        return (
            f"Why: {subject} fails {rate} runs on {self.variant} "
            f"({self.failures} of {self.runs}), so the patch requests that variant."
        )


@dataclass(frozen=True)
class SuiteVariantChoice:
    """The primary variant that justifies selecting one suite, and the reason it was chosen."""

    suite: str
    variant: str
    why: str
    from_context: bool = False
    project: str = PROJECT


@dataclass
class RateContext:
    """Failure-rate evidence, keyed by test."""

    worst: dict[str, RateEvidence] = field(default_factory=dict)
    cpp_cells: dict[str, list[RateEvidence]] = field(default_factory=dict)
    # False when the summary was missing or unreadable, so that an absence of
    # data is never reported as a measured absence of failures.
    available: bool = True


@dataclass
class Guidance:
    """Everything the recommendation logic reads about published test advice."""

    jstest_tests: dict[str, TestGuidance] = field(default_factory=dict)
    cpp_tests: dict[str, TestGuidance] = field(default_factory=dict)
    rates: RateContext = field(default_factory=RateContext)
    accuracy: dict[str, int] = field(default_factory=dict)
    required_variants: set[str] = field(default_factory=set)

    def advice_for(self, target: Target) -> TestGuidance | None:
        """Return the advice for a target, or None if it is outside the analysed scope."""
        tests = self.cpp_tests if target.kind == CPP_UNIT_TEST else self.jstest_tests
        return tests.get(target.name)

    def absent_target_message(self, target: str, kind: str) -> str:
        """Explain that a target was not analysed, which is not the same as being safe."""
        scope = (
            "sheet 16's C++ unit-test binary scope"
            if kind == CPP_UNIT_TEST
            else "sheet 17's CS-owned jstest scope"
        )
        return (
            f"{target}: outside {scope}; no recommendation was produced. "
            "This is not evidence that the target is safe."
        )


def _bracketed_values(body: str, attribute: str) -> Iterator[str]:
    """Yield the text of each ``attribute = [...]`` value, brackets balanced.

    Matching to the first ``]`` is not enough for the ``_select`` attributes,
    whose value is a list of dicts of lists, so the scan tracks depth.
    """
    for match in re.finditer(rf"\b{attribute}\s*=\s*\[", body):
        depth = 0
        start = match.end()
        for index in range(match.end() - 1, len(body)):
            if body[index] == "[":
                depth += 1
            elif body[index] == "]":
                depth -= 1
                if depth == 0:
                    yield body[start:index]
                    break


def _package_path(package: str, name: str) -> str:
    """Join a Bazel package and a name within it; the root package is empty."""
    if not name:
        return package
    return f"{package}/{name}" if package else name


def build_unit_test_source_map(build_path: Path, text: str) -> dict[str, set[str]]:
    """Map source-file paths to the mongo_cc_unit_test binaries that compile them.

    The ``_select`` attributes are read as the union of all their branches. A
    changed file should reach the binary that compiles it under any build
    configuration, and the alternative -- treating a configuration-gated test
    source as production-only -- silently drops it from the recommendation.
    """
    result: dict[str, set[str]] = {}
    # The root BUILD.bazel's parent is ".", which is the empty package.
    package = "" if build_path.parent == Path(".") else build_path.parent.as_posix()
    blocks = re.finditer(
        r"mongo_cc_unit_test\(\s*name\s*=\s*[\"']([^\"']+)[\"'](?P<body>.*?)\n\)",
        text,
        re.DOTALL,
    )
    for match in blocks:
        body = match.group("body")
        source_lists = [
            value for attribute in SOURCE_ATTRIBUTES for value in _bracketed_values(body, attribute)
        ]
        if not source_lists:
            continue
        target = _package_path(package, match.group(1))
        for source_list in source_lists:
            for source in re.findall(r"""["']([^"']+)["']""", source_list):
                # Requiring a source suffix keeps the `//bazel/config:...` and
                # `//conditions:default` keys of a select out of the map.
                if not source.endswith(CPP_FILE_SUFFIXES):
                    continue
                if source.startswith("//"):
                    source_package, _, filename = source[2:].partition(":")
                    path = _package_path(source_package, filename)
                else:
                    # A leading ":" is a label for a file in this same package,
                    # so it names the same path as the bare spelling.
                    path = _package_path(package, source.lstrip(":"))
                result.setdefault(path, set()).add(target)
    return result


def _tracked_build_files(repo: Path) -> list[Path]:
    """Return the repository's tracked BUILD.bazel files.

    Asking git rather than walking the tree keeps nested worktrees, bazel
    symlink forests and build output directories out of the crawl. Walking
    found roughly six times more files than the repository actually has,
    almost all of them copies inside sibling worktrees whose paths can never
    match a changed file.
    """
    completed = subprocess.run(
        ["git", "ls-files", "--", "*BUILD.bazel"],
        cwd=repo,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        return sorted(repo.rglob("BUILD.bazel"))
    return [repo / line.strip() for line in completed.stdout.splitlines() if line.strip()]


def load_unit_test_source_map(repo: Path) -> dict[str, set[str]]:
    """Build the source-to-binary map for the whole worktree."""
    result: dict[str, set[str]] = {}
    for build_path in _tracked_build_files(repo):
        try:
            text = build_path.read_text()
        except OSError:
            continue
        relative = build_path.relative_to(repo)
        for path, targets in build_unit_test_source_map(relative, text).items():
            result.setdefault(path, set()).update(targets)
    return result


def _split_cell(value: str) -> list[str]:
    """Split a pipe-separated sheet cell into its non-empty entries."""
    return [entry.strip() for entry in value.split("|") if entry.strip()]


def _sheet_records(lines: Iterable[str], columns: Sequence[str]) -> Iterator[dict[str, str]]:
    """Yield guidance sheet rows as column-name maps.

    Both sheets carry provenance and blank rows ahead of the header, so the
    header is located by its leading "Test" column rather than by position.
    Columns missing from the sheet yield an empty string.
    """
    rows = list(csv.reader(lines))
    header = next((row for row in rows if row and row[0] == "Test"), None)
    if header is None:
        return
    indexes = {name: header.index(name) if name in header else None for name in columns}
    for row in rows[rows.index(header) + 1 :]:
        record = {
            name: row[index].strip() if index is not None and index < len(row) else ""
            for name, index in indexes.items()
        }
        if record["Test"]:
            yield record


def parse_suite_guidance(lines: Iterable[str]) -> dict[str, TestGuidance]:
    """Parse sheet 17, the suite guidance for all CS-owned jstests."""
    columns = (
        "Test",
        "Basis",
        "Its own BF episodes",
        "Build Baron verified episodes",
        "Episodes behind the advice",
        "Episodes covered",
        "Run it in these suites",
        "Variants it has failed on (context only)",
        "Variants it has failed on",
    )
    result = {}
    for record in _sheet_records(lines, columns):
        # The column has been spelled both ways across revisions of the sheet.
        context = (
            record["Variants it has failed on (context only)"]
            or record["Variants it has failed on"]
        )
        result[record["Test"]] = TestGuidance(
            basis=record["Basis"],
            suites=_split_cell(record["Run it in these suites"]),
            kind=JSTEST,
            sheet=SUITE_SHEET,
            variant_context=_split_cell(context),
            episodes=record["Its own BF episodes"],
            verified=record["Build Baron verified episodes"],
            behind=record["Episodes behind the advice"],
            covered=record["Episodes covered"],
        )
    return result


def parse_variant_guidance(lines: Iterable[str]) -> dict[str, TestGuidance]:
    """Parse sheet 16, the variant guidance for C++ unit-test binaries."""
    columns = (
        "Test",
        "Kind",
        "Failure episodes",
        "Run it in these suites",
        "Variants it has failed on (context only)",
    )
    result = {}
    for record in _sheet_records(lines, columns):
        result[record["Test"]] = TestGuidance(
            basis="own history",
            suites=_split_cell(record["Run it in these suites"]),
            kind=record["Kind"],
            sheet=VARIANT_SHEET,
            variant_context=_split_cell(record["Variants it has failed on (context only)"]),
            episodes=record["Failure episodes"],
        )
    return result


def _read_json(path: Path | None) -> dict:
    """Read a JSON object, treating a missing or unreadable file as empty.

    A truncated or hand-edited artifact must not take the tool down: every
    caller has a defined behaviour for absent guidance, and degrading to that
    is better than a traceback in front of someone trying to submit a patch.
    """
    return _load_json_object(path) or {}


def _load_json_object(path: Path | None) -> dict | None:
    """Read a JSON object, or None if the file is missing, unreadable or not an object."""
    if path is None or not path.exists():
        return None
    try:
        data = json.loads(path.read_text())
    except (OSError, ValueError) as error:
        print(f"Warning: ignoring unreadable guidance artifact {path}: {error}")
        return None
    return data if isinstance(data, dict) else None


def _rate_evidence(cell: dict) -> RateEvidence | None:
    """Build a RateEvidence from one cell of the failure-rate summary."""
    try:
        runs = int(cell["runs"])
        failures = int(cell["failures"])
        variant = cell["variant"]
        task = cell["task"]
    except (KeyError, TypeError, ValueError):
        return None
    raw_project = cell.get("project")
    project = PROJECT if raw_project is None or raw_project == "" else raw_project
    if (
        runs <= 0
        or failures < 0
        or failures > runs
        or not isinstance(variant, str)
        or not variant
        or not isinstance(task, str)
        or not task
        or not isinstance(project, str)
        or not project
    ):
        return None
    return RateEvidence(
        variant=variant,
        task=task,
        runs=runs,
        failures=failures,
        project=project,
    )


def _records(data: dict, key: str) -> list[dict]:
    """Return the object entries of a list section, skipping a malformed section or entry."""
    section = data.get(key)
    if not isinstance(section, list):
        return []
    return [entry for entry in section if isinstance(entry, dict)]


def load_rate_context(path: Path | None) -> RateContext:
    """Read the per-test worst cell and the per-cell C++ evidence from one summary file."""
    data = _load_json_object(path)
    if data is None or not all(
        isinstance(data.get(section), list) for section in ("per_test", "per_cell_failing")
    ):
        return RateContext(available=False)
    context = RateContext()
    for record in _records(data, "per_test"):
        if isinstance(cell := record.get("worst_cell"), dict):
            evidence = _rate_evidence(cell)
            test = record.get("test")
            if evidence is not None and isinstance(test, str) and test:
                context.worst[test] = evidence
    for cell in _records(data, "per_cell_failing"):
        test = cell.get("test")
        if not isinstance(test, str) or not test:
            continue
        # Normalize before filtering: _rate_evidence defaults a missing/empty
        # project to the master project, so the older single-project schema's
        # cells would be rejected by a project check on the raw value before
        # that default could apply, losing all C++ per-cell evidence.
        evidence = _rate_evidence(cell)
        # Nightly cells are kept: their variants are patchable, just in the
        # nightly project rather than the master one.
        if evidence is not None and evidence.project in PROJECT_CONFIGS:
            context.cpp_cells.setdefault(test, []).append(evidence)
    return context


def select_cpp_cells(
    cells: Iterable[RateEvidence],
    required: set[str],
    min_rate: float = CPP_MIN_FAILURE_RATE,
    min_failures: int = CPP_MIN_FAILURES,
) -> list[RateEvidence]:
    """Select non-required cells with repeated, non-negligible evidence."""
    selected = []
    for cell in sorted(cells, key=lambda c: (-c.failures / c.runs, -c.failures)):
        if cell.variant in required:
            continue
        if cell.failures < min_failures:
            continue
        if cell.failures / cell.runs < min_rate:
            break
        selected.append(cell)
    return selected


def _strings(value: object) -> list[str]:
    """Read a string or list of strings, dropping anything else."""
    if isinstance(value, str):
        return [value]
    if isinstance(value, list):
        return [item for item in value if isinstance(item, str)]
    return []


def _entries(data: dict, key: str) -> dict[str, dict]:
    """Return the object-valued entries of a mapping section, skipping malformed ones."""
    section = data.get(key)
    if not isinstance(section, dict):
        return {}
    return {name: value for name, value in section.items() if isinstance(value, dict)}


def _test_guidance_from_json(value: dict, basis: str, kind: str) -> TestGuidance:
    """Build a TestGuidance from one entry of an external guidance JSON file."""

    return TestGuidance(
        basis=value.get("basis", basis),
        suites=_strings(value.get("suites")),
        kind=str(value.get("kind") or kind),
        sheet=str(value.get("sheet", "guidance JSON")),
        variant_context=_strings(value.get("variant_context")),
        episodes=str(value.get("episodes", "")),
        verified=str(value.get("verified", "")),
        behind=str(value.get("behind", "")),
        covered=str(value.get("covered", "")),
    )


def _int_values(values: object) -> dict[str, int]:
    """Keep the integer-valued entries of an accuracy mapping, dropping malformed ones."""
    result = {}
    for key, value in (values if isinstance(values, dict) else {}).items():
        try:
            result[key] = int(value)
        except (TypeError, ValueError):
            print(f"Warning: ignoring non-numeric guidance accuracy {key}={value!r}")
    return result


def _guidance_from_json(path: Path, rate_path: Path | None = None) -> Guidance:
    """Load guidance from the compact JSON schema, with the rate summary beside it."""
    if rate_path is None:
        adjacent = path.parent / RATE_SUMMARY
        rate_path = adjacent if adjacent.exists() else None
    data = _read_json(path)
    guidance = Guidance(
        rates=load_rate_context(rate_path),
        accuracy=_int_values(data.get("accuracy")),
        required_variants=set(_strings(data.get("required_variants"))),
    )
    for test, value in {**_entries(data, "tests"), **_entries(data, "jstest_tests")}.items():
        advice = _test_guidance_from_json(value, "unknown", JSTEST)
        is_cpp = "cpp" in advice.kind.lower() or "binary" in advice.kind.lower()
        (guidance.cpp_tests if is_cpp else guidance.jstest_tests)[test] = advice
    for test, value in _entries(data, "cpp_tests").items():
        guidance.cpp_tests[test] = _test_guidance_from_json(
            value, "own history", "C++ unit test binary"
        )
    return guidance


def _accuracy(holdout_path: Path, sheet_lines: list[str]) -> dict[str, int]:
    """Read holdout accuracy from its JSON file, falling back to the sheet's provenance note."""
    data = _read_json(holdout_path)
    keys = ("at_3_suites_pct", "at_5_suites_pct")
    if accuracy := _int_values({key: data[key] for key in keys if key in data}):
        return accuracy
    note = next(iter(sheet_lines), "")
    three = re.search(r"(\d+)%[^%]*at\s+(?:3|three)\s+suites", note, re.IGNORECASE)
    five = re.search(r"(\d+)%[^%]*at\s+(?:5|five)", note, re.IGNORECASE)
    return {
        key: int(match.group(1))
        for key, match in (("at_3_suites_pct", three), ("at_5_suites_pct", five))
        if match
    }


def _load_sheet_guidance(
    guidance_dir: Path,
    sheet_path: Path | None = None,
    variant_path: Path | None = None,
    holdout_path: Path | None = None,
    rate_path: Path | None = None,
) -> Guidance:
    """Load guidance from the monthly analysis artifacts."""
    sheets = guidance_dir / "sheets_master"
    sheet_path = sheet_path or sheets / SUITE_SHEET
    variant_path = variant_path or sheets / VARIANT_SHEET
    holdout_path = holdout_path or sheets / f"{Path(SUITE_SHEET).stem}.holdout.json"
    rate_path = rate_path or guidance_dir / RATE_SUMMARY

    lines = sheet_path.read_text().splitlines() if sheet_path.exists() else []
    variant_lines = variant_path.read_text().splitlines() if variant_path.exists() else []
    jstest_tests = parse_suite_guidance(lines)
    sheet_16 = parse_variant_guidance(variant_lines)

    # Sheet 16 holds both kinds of target, so route its rows by the kind it
    # records rather than assuming they are all C++ binaries. Sheet 17 covers
    # only jstests, and treating every sheet 16 row as C++ left the jstests
    # that appear in 16 but not 17 unreachable: they resolved to no advice at
    # all and were reported as outside the analysed scope.
    cpp_tests = {test: advice for test, advice in sheet_16.items() if not _is_jstest(test)}
    for test, advice in sheet_16.items():
        if not _is_jstest(test):
            continue
        existing = jstest_tests.get(test)
        if existing is None:
            jstest_tests[test] = advice
        elif not existing.variant_context:
            # Sheet 17 has no variant-context column, so 16 supplies it where
            # both cover the same test.
            existing.variant_context = advice.variant_context
    return Guidance(
        jstest_tests=jstest_tests,
        cpp_tests=cpp_tests,
        rates=load_rate_context(rate_path),
        accuracy=_accuracy(holdout_path, lines),
        required_variants=_required_variants_from_snapshot(guidance_dir),
    )


def _required_variants_from_snapshot(guidance_dir: Path) -> set[str]:
    """Read the required variant set recorded alongside the guidance artifacts."""
    data = _read_json(guidance_dir / "snapshot" / "repo" / "variants.json")
    direct = data.get("required")
    if isinstance(direct, (str, list)):
        return set(_strings(direct))
    effective = data.get("required_effective")
    return set(_strings(effective)) if isinstance(effective, (str, list)) else set()


@contextmanager
def _shallow_clone(repo_url: str, ref: str) -> Iterator[Path]:
    """Yield a shallow clone of a guidance repository, removed on exit."""
    with TemporaryDirectory(prefix="cs-bf-guidance-") as directory:
        subprocess.run(
            ["git", "clone", "--depth", "1", "--branch", ref, repo_url, directory],
            check=True,
            capture_output=True,
            text=True,
        )
        yield Path(directory)


def _validate_sheet_guidance_dir(guidance_dir: Path) -> None:
    """Require both guidance sheets and their Test headers before trusting a checkout."""
    sheets = guidance_dir / "sheets_master"
    for filename in (SUITE_SHEET, VARIANT_SHEET):
        path = sheets / filename
        if not path.exists():
            raise FileNotFoundError(f"Guidance checkout has no {path}.")
        rows = csv.reader(path.read_text().splitlines())
        if not any(row and row[0] == "Test" for row in rows):
            raise GuidanceValidationError(f"Guidance sheet {path} has no Test header.")


def _local_guidance_fallback(guidance_dir: Path | None) -> Guidance | None:
    """Return guidance from a local artifact directory, or None if there is none there."""
    candidate = guidance_dir or REPO / "cs_bf_analysis"
    if (candidate / "sheets_master" / SUITE_SHEET).exists():
        _validate_sheet_guidance_dir(candidate)
        print(f"Warning: using local guidance fallback at {candidate}.")
        return _load_sheet_guidance(candidate)
    return None


def _load_cloned_sheet_guidance(clone: Path) -> Guidance:
    """Load sheet guidance from GUIDANCE_SUBDIR of a guidance repository clone.

    A missing sheet would otherwise load as empty guidance and print a
    confident "no recommendation", so it raises to reach the local fallback.
    """
    guidance_dir = clone / GUIDANCE_SUBDIR
    _validate_sheet_guidance_dir(guidance_dir)
    return _load_sheet_guidance(guidance_dir)


def _cloned_guidance(
    repo_url: str,
    ref: str,
    load: Callable[[Path], Guidance],
    guidance_dir: Path | None,
) -> Guidance:
    """Load guidance from a clone, degrading to local artifacts if the clone fails.

    A missing network or a revoked credential should not stop someone who
    already has the artifacts on disk from getting a recommendation, but it
    must still fail loudly when there is nothing to fall back to.
    """
    try:
        with _shallow_clone(repo_url, ref) as clone:
            return load(clone)
    except (OSError, subprocess.CalledProcessError, GuidanceValidationError):
        fallback = _local_guidance_fallback(guidance_dir)
        if fallback is None:
            raise
        return fallback


def load_guidance(
    guidance_file: Path | None = None,
    guidance_repo: str | None = None,
    guidance_ref: str = GUIDANCE_REF,
    guidance_path: str = "guidance.json",
    guidance_dir: Path | None = None,
    sheet_path: Path | None = None,
    variant_path: Path | None = None,
    holdout_path: Path | None = None,
    rate_path: Path | None = None,
) -> Guidance:
    """Load guidance from the first source the caller supplied, else the default repository."""
    if guidance_file:
        return _guidance_from_json(guidance_file, rate_path)
    if guidance_repo:
        return _cloned_guidance(
            guidance_repo,
            guidance_ref,
            lambda clone: _guidance_from_json(clone / guidance_path, rate_path),
            guidance_dir,
        )
    local_paths = (guidance_dir, sheet_path, variant_path, holdout_path, rate_path)
    if all(path is None for path in local_paths):
        return _cloned_guidance(
            GUIDANCE_REPO,
            GUIDANCE_REF,
            _load_cloned_sheet_guidance,
            guidance_dir,
        )
    return _load_sheet_guidance(
        guidance_dir or Path("."), sheet_path, variant_path, holdout_path, rate_path
    )


def _task_names(task) -> set[str]:
    """Return the names a task can be requested by, including its generated suite names."""
    names = {task.name, task.name.removesuffix("_gen")}
    try:
        names.update(task.get_suite_names())
    except (TypeError, ValueError):
        # Tasks that do not run resmoke have no suites; their own name is enough.
        pass
    return names


def _suite_to_task(repo: Path, project: str) -> dict[str, dict[str, str]]:
    """Map each variant to the task name that runs each suite on it.

    ``evergreen patch -t`` matches task names, not suite names, and the two
    differ often: the guidance advises ``sharding``, but only ``sharding_gen``
    exists as a task, and multiversion tasks name suites that share no prefix
    with the task at all. Sending the suite name selects nothing.

    The mapping has to stay per variant rather than collapse to one task per
    suite. Several tasks can run the same suite -- ``core`` is run by
    ``jsCore``, ``jsCore_in_parts_gen`` and ``config_fuzzer_jsCore`` -- and a
    variant usually carries only one of them, so a globally chosen task can
    have an empty intersection with the variant the guidance picked, silently
    dropping the suite from the patch.
    """
    try:
        config = _parsed_project_config(str(repo), project)
    except EvergreenUnavailable:
        return _suite_to_task_from_components(repo, project)
    return {
        variant.name: _resolve_suite_tasks((task.name, _task_names(task)) for task in variant.tasks)
        for variant in config.variants
    }


def _resolve_suite_tasks(tasks: Iterable[tuple[str, set[str]]]) -> dict[str, str]:
    """Pick the task that each name on one variant should be requested as."""
    candidates: dict[str, set[str]] = {}
    for task_name, names in tasks:
        for name in names:
            candidates.setdefault(name, set()).add(task_name)
    # A name that is itself a task on this variant needs no translation.
    # Otherwise take the shortest task that runs it there, breaking ties by
    # name so the choice is deterministic across runs.
    return {
        name: name if name in found else min(found, key=lambda task: (len(task), task))
        for name, found in candidates.items()
    }


def _suite_to_task_from_components(repo: Path, project: str) -> dict[str, dict[str, str]]:
    """Build the per-variant suite-to-task map from the component YAML files.

    ``variant_tasks`` falls back to the components when the evergreen CLI is
    unavailable, so this has to as well: returning nothing here would leave
    every suite requested under its own name, and a generated suite such as
    ``sharding`` selects no task, which is exactly the failure the mapping
    exists to prevent.
    """
    definitions = _declared_task_names(repo, project)
    return {
        variant: _resolve_suite_tasks(
            (name, _fallback_task_names(name, definitions)) for name in names
        )
        for variant, names in _declared_variant_tasks(repo, project).items()
    }


def _requestable_task(
    suite: str,
    variants: Iterable[str],
    suite_to_task: dict[str, dict[str, str]],
) -> str:
    """Return the task name that runs a suite on the first variant that has it.

    ``variants`` is the set the patch will request, so resolving within it is
    what keeps ``-v`` and ``-t`` intersecting. A suite no listed variant runs
    falls back to its own name rather than borrowing another variant's task.
    """
    for variant in variants:
        task = suite_to_task.get(variant, {}).get(suite)
        if task is not None:
            return task
    return suite


def _included_component_paths(repo: Path, project: str) -> list[Path]:
    """Return the component files a project's root config actually includes.

    Crawling the component directories instead would mix the projects up in
    both directions: it gives master the release-only variants that live under
    ``variants/*/test_release.yml``, and it denies nightly the variants it
    includes from outside that tree, such as ``custom_builds/variants.yml``.
    Either way a variant can be attributed to a project that does not have it,
    and a patch naming it is rejected.
    """
    root = yaml.safe_load(project_config_path(repo, project).read_text()) or {}
    if not isinstance(root, dict):
        raise ValueError(f"Evergreen root configuration for {project} is not a mapping.")
    includes = root.get("include") or []
    if not isinstance(includes, list):
        raise ValueError(f"Evergreen root configuration for {project} has a non-list include.")
    paths = []
    for entry in includes:
        filename = entry.get("filename") if isinstance(entry, dict) else entry
        if not filename:
            continue
        path = repo / filename
        if not path.exists():
            raise FileNotFoundError(
                f"Evergreen root configuration for {project} includes missing component {path}."
            )
        paths.append(path)
    return paths


def _component_document(path: Path) -> dict:
    """Parse one component YAML file without hiding configuration errors."""
    document = yaml.safe_load(path.read_text())
    if document is None:
        return {}
    if not isinstance(document, dict):
        raise ValueError(f"Evergreen component {path} is not a mapping.")
    return document


def _declared_task_definitions(repo: Path, project: str) -> dict[str, dict]:
    """Return the raw definition of each task a project's included components define."""
    result: dict[str, dict] = {}
    for path in _included_component_paths(repo, project):
        for raw in _component_document(path).get("tasks") or []:
            if isinstance(raw, dict) and isinstance(raw.get("name"), str):
                result[raw["name"]] = raw
    return result


def _declared_task_names(repo: Path, project: str) -> dict[str, set[str]]:
    """Map each task a project's components define to the names it can be requested by.

    The suite comes from the task definition, not its spelling: ``jsCore`` runs
    suite ``core``, which no ``_gen`` stripping can recover.
    """
    result: dict[str, set[str]] = {}
    for name, raw in _declared_task_definitions(repo, project).items():
        try:
            result[name] = _task_names(Task(raw))
        except AttributeError:
            # A malformed command list; fall back to the name's spelling.
            continue
    return result


def _expand_task_selector(
    selector: str,
    tags_by_task: dict[str, set[str]],
    groups: dict[str, list[str]] | None = None,
    seen_groups: frozenset[str] = frozenset(),
) -> set[str]:
    """Expand an Evergreen task selector into the task names it matches.

    A selector is space-separated criteria that must all hold: ``.tag`` for a
    tag, a bare task or task-group name, or ``*``; a leading ``!`` negates one.
    Taking ``.development_critical !.requires_large_host`` as a literal name
    matched nothing and dropped most of each variant's matrix. Task groups can
    carry the same filters, for example ``run_unit_tests_TG !.multiversion``,
    so they must be expanded before the remaining criteria are applied.
    """
    groups = groups or {}

    def group_members(name: str) -> set[str]:
        if name in seen_groups:
            return set()
        return {
            task
            for member in groups.get(name, [])
            for task in _expand_task_selector(
                str(member), tags_by_task, groups, seen_groups | {name}
            )
        }

    criteria = selector.split()
    if len(criteria) == 1 and not criteria[0].startswith((".", "!")) and criteria[0] != "*":
        # A plain name, kept even without a definition: compile tasks and task
        # groups are requested by name but live outside the parsed task lists.
        return group_members(criteria[0]) if criteria[0] in groups else {criteria[0]}
    matched = set(tags_by_task)
    for criterion in criteria:
        negated = criterion.startswith("!")
        term = criterion.removeprefix("!")
        if term == "*":
            hits = set(tags_by_task)
        elif term.startswith("."):
            tag = term[1:]
            hits = {task for task, tags in tags_by_task.items() if tag in tags}
        elif term in groups:
            hits = group_members(term)
        else:
            hits = {term} & set(tags_by_task)
        matched = matched - hits if negated else matched & hits
    return matched


def _fallback_task_names(name: str, definitions: dict[str, set[str]]) -> set[str]:
    """Return a declared task's names, guessing from its spelling if it has no definition."""
    return definitions.get(name) or {name, name.removesuffix("_gen")}


def _declared_variant_tasks(repo: Path, project: str) -> dict[str, set[str]]:
    """Return the task names each variant declares, as the component files spell them.

    This is the raw reading: the names are exactly the configured task names,
    with task groups and tag selectors expanded but no ``_gen`` aliasing. Both
    the task matrix and the suite-to-task fallback build on it, and the latter
    needs the real spelling to know what a patch can request.
    """
    paths = _included_component_paths(repo, project)
    tags_by_task = {
        name: set(_strings(raw.get("tags")))
        for name, raw in _declared_task_definitions(repo, project).items()
    }
    groups: dict[str, list[str]] = {}
    for path in paths:
        document = _component_document(path)
        for group in document.get("task_groups") or []:
            if isinstance(group, dict) and group.get("name"):
                groups[group["name"]] = list(group.get("tasks") or [])

    result: dict[str, set[str]] = {}
    for path in paths:
        document = _component_document(path)
        for variant in document.get("buildvariants") or []:
            if not isinstance(variant, dict) or not variant.get("name"):
                continue
            tasks = result.setdefault(variant["name"], set())
            for task in variant.get("tasks") or []:
                name = task.get("name") if isinstance(task, dict) else str(task)
                if not name:
                    continue
                tasks.update(_expand_task_selector(name, tags_by_task, groups))
    return result


def _required_variants_from_components(repo: Path, project: str) -> set[str]:
    """Read the current required set from included buildvariant display names."""
    required = set()
    for path in _included_component_paths(repo, project):
        for variant in _component_document(path).get("buildvariants") or []:
            if not isinstance(variant, dict):
                continue
            name = variant.get("name")
            display_name = variant.get("display_name")
            if (
                isinstance(name, str)
                and name
                and isinstance(display_name, str)
                and display_name.startswith("!")
            ):
                required.add(name)
    return required


def _variant_task_counts_from_components(repo: Path, project: str) -> dict[str, int]:
    """Count actual configured tasks per variant without counting matching aliases."""
    return {
        variant: len(tasks) for variant, tasks in _declared_variant_tasks(repo, project).items()
    }


def _variant_tasks_from_components(
    repo: Path,
    project: str = PROJECT,
) -> dict[str, set[str]]:
    """Expand the variant matrix from the component YAML files.

    This is the fallback for environments without the evergreen CLI. Tag
    selectors are expanded from the included task definitions, but display
    tasks and other evaluation-time features are not modelled.
    """
    definitions = _declared_task_names(repo, project)
    return {
        variant: {alias for name in names for alias in _fallback_task_names(name, definitions)}
        for variant, names in _declared_variant_tasks(repo, project).items()
    }


def project_config_path(repo: Path, project: str) -> Path:
    """Resolve the project config without silently returning an empty matrix."""
    relative = PROJECT_CONFIGS.get(project)
    if relative:
        return repo / relative
    fallback = repo / "etc/evergreen.yml"
    if fallback.exists():
        return fallback
    raise ValueError(f"No Evergreen project configuration is available for {project}.")


@lru_cache(maxsize=None)
def _parsed_project_config(repo: str, project: str):
    """Parse one project configuration once per process."""
    evergreen_binary = _evergreen_binary()
    if not evergreen_binary:
        raise EvergreenUnavailable(project)
    return parse_evergreen_file(
        str(project_config_path(Path(repo), project)), evergreen_binary=evergreen_binary
    )


@lru_cache(maxsize=None)
def _evergreen_binary() -> str | None:
    """Return the evergreen CLI path, warning once if it is missing."""
    try:
        return find_evergreen_binary("evergreen")
    except OSError:
        print(
            "Warning: evergreen CLI not found; approximating the variant matrix from the "
            "component YAML files, which may miss tasks."
        )
        return None


def variant_tasks(repo: Path, project: str = PROJECT) -> dict[str, set[str]]:
    """Return the tasks each Evergreen variant of a project runs.

    The source YAML contains selector expressions such as `.concurrency`, so the
    evergreen CLI is the authoritative way to expand the matrix. Project-specific
    component YAML files are the fallback when the CLI is unavailable.
    """
    try:
        config = _parsed_project_config(str(repo), project)
    except EvergreenUnavailable:
        return _variant_tasks_from_components(repo, project)
    return {
        variant.name: {name for task in variant.tasks for name in _task_names(task)}
        for variant in config.variants
    }


def variant_task_counts(repo: Path, project: str = PROJECT) -> dict[str, int]:
    """Return each variant's actual task count, excluding suite and spelling aliases."""
    try:
        config = _parsed_project_config(str(repo), project)
    except EvergreenUnavailable:
        return _variant_task_counts_from_components(repo, project)
    return {variant.name: len({task.name for task in variant.tasks}) for variant in config.variants}


def required_variants(repo: Path, project: str = PROJECT) -> set[str]:
    """Return the current required variants, using components only when the CLI is absent."""
    try:
        config = _parsed_project_config(str(repo), project)
    except EvergreenUnavailable:
        return _required_variants_from_components(repo, project)
    return {variant.name for variant in config.get_required_variants()}


def choose_suite_variants(
    suites: Iterable[str],
    variant_tasks: dict[str, set[str]],
    context_variants: set[str] | None = None,
    variant_costs: dict[str, int] | None = None,
    required_variants: set[str] | None = None,
    project: str = PROJECT,
    context_only: bool = False,
    context_by_suite: dict[str, set[str]] | None = None,
    evidence_by_variant: dict[str, int] | None = None,
) -> list[SuiteVariantChoice]:
    """Pick one primary variant per suite, preferring prior failure variants.

    These choices justify which variants enter the patch; they are not exact
    variant/task scheduling pairs. The generated Evergreen command unions all
    selected variants and tasks, so Evergreen can run any selected task on any
    selected variant where it exists.

    Ordering depends on whether actionable context exists, and the two cases
    want opposite treatment of the required set:

    * Among failure-context candidates, a required variant ranks last even when
      it is cheapest. The patch runs it anyway, so choosing it would drop the
      one variant that adds coverage. Among the rest the variant with the most
      recorded failures wins, with cost and name breaking ties, so the choice
      is deterministic without being cost-led.
    * With no context to act on, a required variant ranks first. The suite is
      already covered by the alias, and adding an arbitrary optional variant
      buys nothing while lengthening the patch.

    With ``context_only``, suites are skipped unless a failure-context variant
    runs them. That is what the nightly pass wants: it should add a variant only
    where the history points at one, never a cheapest-available fallback.
    """
    context_variants = context_variants or set()
    context_by_suite = context_by_suite or {}
    required_variants = required_variants or set()
    evidence_by_variant = evidence_by_variant or {}
    variant_costs = variant_costs or {
        variant: len(tasks) for variant, tasks in variant_tasks.items()
    }

    def rank(candidate: str, actionable: bool) -> tuple:
        required = candidate in required_variants
        # With context to act on, a required variant is the least useful pick;
        # without it, the required variant is the one that costs nothing.
        return (
            required if actionable else not required,
            -evidence_by_variant.get(candidate, 0),
            variant_costs.get(candidate, 0),
            candidate,
        )

    selected: list[SuiteVariantChoice] = []
    for suite in suites:
        suite_names = _task_aliases(suite)
        candidates = [variant for variant, tasks in variant_tasks.items() if suite_names & tasks]
        if not candidates:
            continue
        preferred_context = context_by_suite.get(suite, context_variants)
        preferred = [variant for variant in candidates if variant in preferred_context]
        if context_only and not preferred:
            continue
        variant = min(preferred or candidates, key=lambda name: rank(name, bool(preferred)))
        if variant in required_variants:
            why = (
                f"Why: {variant} is already in the required set for suite {suite}; "
                "no extra variant is needed."
            )
        elif preferred:
            why = (
                f"Why: {variant} is a variant where a changed test has historical "
                f"failure context for suite {suite}."
            )
        else:
            why = (
                f"Why: {variant} is the lowest-task-count current variant running "
                f"suite {suite}; no failure-context variant was available."
            )
        # from_context drives whether the nightly pass still looks at this
        # suite, so it must mean "a failure-context variant will actually run",
        # not merely "one existed". A suite whose only context variants are
        # required is genuinely covered by the required set and needs nothing
        # more, which is why required counts here.
        covered_by_context = variant in preferred
        selected.append(SuiteVariantChoice(suite, variant, why, covered_by_context, project))
    return selected


def changed_files(repo: Path, base_ref: str = BASE_REF) -> list[str]:
    """Return the files this change touches, committed or not.

    Untracked files are deliberately excluded. ``evergreen patch -u`` submits
    the working tree diff against HEAD, which does not carry untracked files,
    so recommending coverage for a brand-new test would advertise testing the
    patch cannot perform. A new test starts being considered once it is staged.
    """
    result: set[str] = set()
    commands = [
        ["git", "diff", "--name-only", "--diff-filter=ACMRTUXB", f"{base_ref}...HEAD"],
        ["git", "diff", "--name-only", "--diff-filter=ACMRTUXB"],
        ["git", "diff", "--cached", "--name-only", "--diff-filter=ACMRTUXB"],
    ]
    for command in commands:
        completed = subprocess.run(command, cwd=repo, capture_output=True, text=True)
        if completed.returncode != 0:
            # Treating a failed diff as an empty one is the worst outcome here:
            # a missing origin/master would silently reduce a whole branch of
            # committed tests to "no changed files found".
            raise RuntimeError(
                f"{shlex.join(command)} failed in {repo}: "
                f"{completed.stderr.strip() or completed.returncode}"
            )
        result.update(line.strip() for line in completed.stdout.splitlines() if line.strip())
    return sorted(result)


def _is_jstest(path: str) -> bool:
    """Return True if a path is a jstest."""
    return path.endswith(".js") and (path.startswith("jstests/") or "/jstests/" in path)


def collect_targets(
    files: Iterable[str],
    source_map: dict[str, set[str]],
) -> tuple[list[Target], list[str]]:
    """Split changed files into test targets and ignored production-only C++ changes."""
    targets: list[Target] = []
    seen_targets: set[tuple[str, str]] = set()
    ignored_cpp: list[str] = []
    for path in files:
        if _is_jstest(path):
            candidates = [Target(path, JSTEST)]
        elif path in source_map:
            candidates = [Target(target, CPP_UNIT_TEST) for target in sorted(source_map[path])]
        elif path.endswith(CPP_FILE_SUFFIXES):
            ignored_cpp.append(path)
            candidates = []
        else:
            candidates = []
        for target in candidates:
            key = (target.name, target.kind)
            if key not in seen_targets:
                seen_targets.add(key)
                targets.append(target)
    return targets, ignored_cpp


def recommended_suites(targets: Iterable[Target], guidance: Guidance) -> list[str]:
    """Return the suites advised for the given targets."""
    return sorted(
        {
            suite
            for target in targets
            for suite in (guidance.advice_for(target) or TestGuidance("", [])).suites
        }
    )


def _task_runs_on_variant(task: str, variant_tasks: dict[str, set[str]]) -> bool:
    """Return True if any of the given variants runs the task under either spelling."""
    names = _task_aliases(task)
    return any(names & tasks for tasks in variant_tasks.values())


def alias_for(project: str) -> str | None:
    """Return the patch alias a project takes, which the nightly project does not."""
    return None if project == NIGHTLY_PROJECT else "required"


def build_evergreen_command(
    variants: Iterable[str],
    tasks: Iterable[str],
    project: str = PROJECT,
    alias: str | None = "required",
    evergreen_binary: str = "evergreen",
) -> list[str]:
    """Build the evergreen patch command for the recommendation.

    The alias is explicit because the nightly patch does not take one. Hiding
    that behind a project comparison made nightly-required variants fall
    through a gap: they were withheld from ``-v`` as already covered, while the
    nightly command had no alias to cover them, so they ran in no patch at all.

    Evergreen treats the variant and task arguments as independent filters.
    This command intentionally submits their union, producing cross-coverage:
    each selected task may run on every selected variant where it is configured.
    """
    command = [evergreen_binary, "patch", "-p", project]
    if alias:
        command.extend(["-a", alias])
    if variant_list := sorted(set(variants)):
        command.extend(["-v", ",".join(variant_list)])
    if task_list := sorted(set(tasks)):
        command.extend(["-t", ",".join(task_list)])
    command.extend(["-f", "-y", "-u"])
    return command


@dataclass
class Recommendation:
    """What to run for a set of changed targets, and why."""

    targets: list[Target]
    ignored_cpp: list[str]
    suites: list[str]
    variants: list[str]
    js_choices: list[SuiteVariantChoice]
    cpp_cells: dict[str, list[RateEvidence]]
    # The master project's required set, which its patch runs through the alias.
    required: set[str]
    command: list[str]
    # What counts as already required when describing each project's cells.
    # Nightly is empty: its patch has no alias, so nothing there is covered.
    required_by_project: dict[str, set[str]] = field(default_factory=dict)
    nightly_variants: list[str] = field(default_factory=list)
    nightly_command: list[str] = field(default_factory=list)
    nightly_js_choices: list[SuiteVariantChoice] = field(default_factory=list)
    # Suites the guidance advises but that no variant the patch requests runs,
    # so they reach neither command. Reported rather than dropped silently.
    unschedulable_suites: list[str] = field(default_factory=list)
    # Cells that cleared the rate thresholds but whose variant no longer runs
    # their task. They were rejected for reachability, not for being weak, and
    # the report has to say so rather than misdescribe their rate.
    unreachable_cpp_cells: dict[str, list[RateEvidence]] = field(default_factory=dict)


def _select_cpp_cells(
    cells: Iterable[RateEvidence],
    scopes: dict[str, set[str]],
) -> list[RateEvidence]:
    """Select the cells worth acting on, judging each project's cells on their own.

    ``scopes`` maps a project to the variants already required there. Cells
    from a project with no scope are dropped: neither patch can request them.

    Reachability is deliberately not applied here. A cell dropped because its
    variant is gone was not rejected on strength, and the report has to tell
    those two cases apart, so ``_runnable_cells`` makes that split afterwards
    and checks the task as well as the variant.
    """
    ordered = list(cells)
    by_project: dict[str, list[RateEvidence]] = {}
    for cell in ordered:
        if cell.project in scopes:
            by_project.setdefault(cell.project, []).append(cell)
    chosen: set[int] = set()
    for cell_project, project_cells in by_project.items():
        selected = select_cpp_cells(project_cells, scopes[cell_project])
        chosen.update(id(cell) for cell in selected)
    # Report in the order the guidance artifact recorded the cells, not in the
    # rate order select_cpp_cells ranks them by.
    return [cell for cell in ordered if id(cell) in chosen]


def _context_by_suite_test(
    js_targets: Iterable[Target],
    guidance: Guidance,
) -> dict[tuple[str, str], set[str]]:
    """Map each (suite, test) pair to the variants that test has failed on.

    Context is tracked per pair rather than unioned per suite. Two changed
    tests can advise the same suite for different reasons: if one is already
    covered by a required variant, a union marks the suite covered and the
    other test's evidence, which may only exist on a nightly-only variant,
    never gets a nightly pass.
    """
    result: dict[tuple[str, str], set[str]] = {}
    for target in js_targets:
        advice = guidance.advice_for(target)
        if not advice:
            continue
        for suite in advice.suites:
            result.setdefault((suite, target.name), set()).update(advice.variant_context)
    return result


def _runnable_cells(
    cells: Iterable[RateEvidence],
    variant_tasks_by_project: dict[str, dict[str, set[str]]],
) -> list[RateEvidence]:
    """Keep the cells whose variant still exists and still runs their task.

    A variant renamed or retired since the analysis window, or one that has
    stopped running the task, cannot reproduce the failure, so naming it would
    put a selection in the patch command that Evergreen matches nothing for.
    """
    runnable = []
    for cell in cells:
        variant_tasks = variant_tasks_by_project.get(cell.project) or {}
        tasks = variant_tasks.get(cell.variant)
        if tasks is not None and _task_runs_on_variant(cell.task, {cell.variant: tasks}):
            runnable.append(cell)
    return runnable


def _evidence_by_variant(
    js_targets: Iterable[Target],
    guidance: Guidance,
    project: str,
) -> dict[str, int]:
    """Per-variant failure strength, restricted to cells measured on ``project``."""
    result: dict[str, int] = {}
    for target in js_targets:
        cell = guidance.rates.worst.get(target.name)
        if cell is not None and cell.project == project:
            result[cell.variant] = max(result.get(cell.variant, 0), cell.failures)
    return result


def recommend(
    targets: list[Target],
    ignored_cpp: list[str],
    guidance: Guidance,
    variant_tasks: dict[str, set[str]],
    required: set[str],
    project: str = PROJECT,
    nightly_variant_tasks: dict[str, set[str]] | None = None,
    suite_to_task: dict[str, dict[str, str]] | None = None,
    nightly_suite_to_task: dict[str, dict[str, str]] | None = None,
    evergreen_binary: str = "evergreen",
    variant_costs: dict[str, int] | None = None,
    nightly_variant_costs: dict[str, int] | None = None,
) -> Recommendation:
    """Turn changed targets into the suites, variants and patch commands to run.

    Evidence measured on nightly-only variants is reported separately, because
    those variants exist in the nightly project and cannot be requested from a
    patch against ``project``. That applies to both the C++ rate evidence and
    the jstest failure context.

    Suite names are reported as the guidance spells them, but the patch
    commands carry the task names those suites map to, which is what
    ``evergreen patch -t`` matches.
    """
    nightly_variant_tasks = nightly_variant_tasks or {}
    suite_to_task = suite_to_task or {}
    nightly_suite_to_task = nightly_suite_to_task or {}
    js_targets = [target for target in targets if target.kind == JSTEST]
    cpp_targets = [target for target in targets if target.kind == CPP_UNIT_TEST]

    js_suites = recommended_suites(js_targets, guidance)
    cpp_suites = recommended_suites(cpp_targets, guidance)
    # Context is tracked per (suite, test) rather than unioned per suite. Two
    # changed tests can advise the same suite for different reasons: if one is
    # already covered by a required variant, a union marks the suite covered
    # and the other test's evidence, which may only exist on a nightly-only
    # variant, never gets a nightly pass.
    context_by_suite_test = _context_by_suite_test(js_targets, guidance)
    context_by_suite: dict[str, set[str]] = {}
    for (suite, _), context in context_by_suite_test.items():
        context_by_suite.setdefault(suite, set()).update(context)
    # The sheets record which variants a test failed on but not how often, so
    # the worst measured cell is the only per-variant strength available. It
    # ranks candidates that would otherwise be separated by cost alone. Cells
    # measured on another project are excluded: a variant name shared between
    # projects would otherwise carry nightly evidence into the master ranking.
    js_choices = choose_suite_variants(
        js_suites,
        variant_tasks,
        required_variants=required,
        project=project,
        context_by_suite=context_by_suite,
        evidence_by_variant=_evidence_by_variant(js_targets, guidance, project),
        variant_costs=variant_costs,
    )
    # A master choice covers a test's historical context only when that exact
    # test names the chosen variant. Keeping this association prevents test A's
    # master context from hiding test B's nightly evidence when they share a
    # suite.
    choice_by_suite = {choice.suite: choice for choice in js_choices}
    nightly_context_by_suite: dict[str, set[str]] = {}
    for (suite, _), context in context_by_suite_test.items():
        master_choice = choice_by_suite.get(suite)
        if master_choice is not None and master_choice.variant in context:
            continue
        nightly_context = context & set(nightly_variant_tasks)
        if nightly_context:
            nightly_context_by_suite.setdefault(suite, set()).update(nightly_context)
    nightly_js_choices = choose_suite_variants(
        sorted(nightly_context_by_suite),
        nightly_variant_tasks,
        # The nightly patch carries no required alias, so a nightly-required
        # variant is not already covered there. Passing the nightly required
        # set would rank such a variant behind weaker optional candidates and
        # emit the false "no extra variant is needed" reason. Treat it as empty
        # for selection, as the C++ path does with scopes[NIGHTLY_PROJECT].
        required_variants=set(),
        project=NIGHTLY_PROJECT,
        context_only=True,
        context_by_suite=nightly_context_by_suite,
        evidence_by_variant=_evidence_by_variant(js_targets, guidance, NIGHTLY_PROJECT),
        variant_costs=nightly_variant_costs,
    )

    # A variant is "already required", and a variant exists at all, only within
    # its own project. Judging a master cell against the union of both projects
    # would drop it for being required in nightly, discarding evidence the
    # master patch would have used. The nightly entry is written second so it
    # wins when the patch itself targets the nightly project.
    #
    # Nothing is "already required" in nightly for selection purposes: that
    # patch carries no alias, so a nightly-required variant is scheduled only
    # if this evidence puts it there. Discarding a cell for being required
    # would leave it running in neither patch, which is the same reasoning the
    # jstest path already applies to nightly_variants.
    scopes = {project: required}
    if project in PROJECT_CONFIGS:
        scopes[NIGHTLY_PROJECT] = set()
    # Only cells whose variant still runs their task are kept. Storing the
    # threshold-selected cells instead would have the report claim a variant
    # was added on evidence the command then, correctly, omits.
    matrices = {project: variant_tasks, NIGHTLY_PROJECT: nightly_variant_tasks}
    qualifying = {
        target.name: _select_cpp_cells(guidance.rates.cpp_cells.get(target.name, []), scopes)
        for target in cpp_targets
    }
    cpp_cells = {target: _runnable_cells(cells, matrices) for target, cells in qualifying.items()}
    unreachable_cpp_cells = {
        target: [cell for cell in cells if id(cell) not in {id(kept) for kept in cpp_cells[target]}]
        for target, cells in qualifying.items()
    }
    selected_cells = [cell for cells in cpp_cells.values() for cell in cells]
    project_cells = [cell for cell in selected_cells if cell.project == project]
    nightly_cells = [cell for cell in selected_cells if cell.project == NIGHTLY_PROJECT]

    variants = sorted(
        {choice.variant for choice in js_choices if choice.variant not in required}
        | {cell.variant for cell in project_cells}
    )
    # Unlike the master patch, the nightly one carries no alias, so nothing
    # else schedules a nightly-required variant. Withholding it here would
    # leave it running in neither patch.
    nightly_variants = sorted(
        {choice.variant for choice in nightly_js_choices} | {cell.variant for cell in nightly_cells}
    )
    # A C++ suite counts as scheduled only where a variant the patch actually
    # requests runs it. Checking the whole project matrix instead would call a
    # suite scheduled because some optional variant runs it, while the command
    # names neither that variant nor anything else that could match the task.
    # The master patch also runs the required set through its alias.
    requested = {
        name: variant_tasks[name] for name in set(variants) | required if name in variant_tasks
    }
    requested_nightly = {
        name: nightly_variant_tasks[name]
        for name in nightly_variants
        if name in nightly_variant_tasks
    }
    scheduled_suites = {suite for suite in cpp_suites if _task_runs_on_variant(suite, requested)}
    scheduled_nightly_suites = {
        suite for suite in cpp_suites if _task_runs_on_variant(suite, requested_nightly)
    }
    # The suites are reported under their guidance names, but requested under
    # the task names Evergreen matches, which are often not the same string.
    # Each name is resolved against the variants that will actually run it, so
    # that -v and -t intersect: several tasks can run one suite, and a variant
    # usually carries only one of them. The master patch also runs the required
    # variants through its alias, so they count as requested there.
    # A cell's task comes from the analysis artifact, which records the suite
    # spelling, so it needs the same resolution against its own variant as a
    # jstest suite does.
    tasks = {_requestable_task(cell.task, [cell.variant], suite_to_task) for cell in project_cells}
    for choice in js_choices:
        tasks.add(_requestable_task(choice.suite, [choice.variant], suite_to_task))
    for suite in scheduled_suites:
        tasks.add(_requestable_task(suite, sorted(set(variants) | required), suite_to_task))
    nightly_tasks = {
        _requestable_task(cell.task, [cell.variant], nightly_suite_to_task)
        for cell in nightly_cells
    }
    for choice in nightly_js_choices:
        nightly_tasks.add(_requestable_task(choice.suite, [choice.variant], nightly_suite_to_task))
    for suite in scheduled_nightly_suites:
        nightly_tasks.add(_requestable_task(suite, nightly_variants, nightly_suite_to_task))
    tasks = sorted(tasks)
    nightly_tasks = sorted(nightly_tasks)
    scheduled_suites |= {choice.suite for choice in js_choices}
    scheduled_nightly_suites |= {choice.suite for choice in nightly_js_choices}
    scheduled = scheduled_suites | scheduled_nightly_suites
    all_suites = recommended_suites(targets, guidance)
    return Recommendation(
        targets=targets,
        ignored_cpp=ignored_cpp,
        suites=all_suites,
        unschedulable_suites=[suite for suite in all_suites if suite not in scheduled],
        variants=variants,
        js_choices=js_choices,
        cpp_cells=cpp_cells,
        unreachable_cpp_cells=unreachable_cpp_cells,
        required=required,
        required_by_project=scopes,
        command=build_evergreen_command(
            variants, tasks, project, alias=alias_for(project), evergreen_binary=evergreen_binary
        ),
        nightly_variants=nightly_variants,
        nightly_command=(
            build_evergreen_command(
                nightly_variants,
                nightly_tasks,
                NIGHTLY_PROJECT,
                alias=None,
                evergreen_binary=evergreen_binary,
            )
            if nightly_variants
            else []
        ),
        nightly_js_choices=nightly_js_choices,
    )


def _qualifies(cell: RateEvidence) -> bool:
    """Return True if a cell clears both the repeated-evidence and rate thresholds."""
    return cell.failures >= CPP_MIN_FAILURES and cell.failures / cell.runs >= CPP_MIN_FAILURE_RATE


def describe_unreachable(cells: list[RateEvidence]) -> str:
    """Describe evidence that no longer maps onto the current configuration.

    These cells were not rejected on their rate, so the rate wording must not
    be applied to them: a 20-of-100 cell on a retired variant is strong
    evidence that simply has nowhere left to run, and calling it too rare to
    reproduce would be false.
    """
    failures = sum(cell.failures for cell in cells)
    variants = sorted({cell.variant for cell in cells})
    named = ", ".join(variants[:3]) + (", ..." if len(variants) > 3 else "")
    return (
        f"{failures} {'failure' if failures == 1 else 'failures'} recorded on "
        f"{len(variants)} {'variant' if len(variants) == 1 else 'variants'} "
        f"({named}) that no longer run this task in the current configuration; "
        "the evidence cannot be reproduced by a patch, so no extra variant was added."
    )


def describe_unselected(cells: list[RateEvidence], required: set[str]) -> str:
    """Describe what the evidence shows when it justifies no extra variant.

    The point is to state what is true of the measurements, not that a
    threshold went unmet, so that a reader can judge the risk themselves.
    """
    if not cells:
        return "no failures recorded for this binary in the analysis window; nothing to target."

    failures = sum(cell.failures for cell in cells)
    variants = len({cell.variant for cell in cells})
    spread = (
        f"{failures} {'failure' if failures == 1 else 'failures'} across {variants} "
        f"{'variant' if variants == 1 else 'variants'}"
    )

    if qualifying_required := [
        cell for cell in cells if cell.variant in required and _qualifies(cell)
    ]:
        best = max(qualifying_required, key=lambda cell: cell.failures / cell.runs)
        return (
            f"{spread}; the strongest reproducible signal ({best.failures} of {best.runs} on "
            f"{best.variant}) is already in the required set, so no extra variant is needed."
        )

    # Every non-required cell was blocked either for being a lone failure or for
    # being too rare to reproduce. Name whichever bound the best candidate.
    candidates = [cell for cell in cells if cell.variant not in required]
    if not candidates:
        return (
            f"{spread}; no non-required evidence was available to justify an "
            "extra variant, so no extra variant was added."
        )
    repeated = [cell for cell in candidates if cell.failures >= CPP_MIN_FAILURES]
    if repeated:
        best = max(repeated, key=lambda cell: cell.failures / cell.runs)
        reason = (
            f"the best repeated cell is {best.failures} of {best.runs} "
            f"({best.failures / best.runs:.2%}) on {best.variant}, under the "
            f"{CPP_MIN_FAILURE_RATE:.0%} rate a patch run could hope to reproduce"
        )
    else:
        best = max(candidates, key=lambda cell: cell.failures / cell.runs)
        reason = (
            f"no variant failed more than once, the worst being 1 of {best.runs} on "
            f"{best.variant}, which is indistinguishable from noise"
        )
    return f"{spread}, none reproducible on a single variant: {reason}; no extra variant added."


def _print_cpp_evidence(
    target: Target,
    guidance: Guidance,
    selected: list[RateEvidence],
    required_by_project: dict[str, set[str]],
    unreachable: list[RateEvidence] | None = None,
) -> None:
    """Print the rate evidence that selected, or failed to select, extra variants."""
    if not selected:
        if not guidance.rates.available:
            print("    failure-rate summary unavailable; rate-based variants could not be chosen.")
            return
        if unreachable:
            # These cells passed the rate thresholds and were dropped only
            # because their variant no longer runs the task, so the
            # rate-based wording below would misdescribe them.
            print(f"    {describe_unreachable(unreachable)}")
            return
        # Describe each project separately. Summing across them reports a
        # spread and a rate for a population that never ran together, which
        # overstates how reproducible the failure is in either place.
        cells = guidance.rates.cpp_cells.get(target.name, [])
        by_project: dict[str, list[RateEvidence]] = {}
        for cell in cells:
            by_project.setdefault(cell.project, []).append(cell)
        if not by_project:
            print(f"    {describe_unselected([], set())}")
            return
        for cell_project, project_cells in sorted(by_project.items()):
            label = "" if len(by_project) == 1 else f"on {cell_project}: "
            project_required = required_by_project.get(cell_project, set())
            print(f"    {label}{describe_unselected(project_cells, project_required)}")
        return
    subject = target.name.rsplit("/", 1)[-1]
    print("    rate-selected cells:")
    for cell in selected:
        print(f"      {cell.summary()}")
        print(f"      {cell.why(subject)}")
    if unreachable:
        # Still worth surfacing: a strong cell lost to a variant rename is a
        # coverage gap even when another variant was selected.
        print(f"    also: {describe_unreachable(unreachable)}")


def _print_variants(variants: Iterable[str], choices: Iterable[SuiteVariantChoice]) -> None:
    """Print each selected variant with the per-suite reasons that chose it."""
    choices = list(choices)
    for variant in variants:
        print(f"  {variant}")
        for choice in choices:
            if choice.variant == variant:
                print(f"    {choice.why}")


def _print_report(recommendation: Recommendation, guidance: Guidance) -> None:
    """Print the recommendation and the evidence behind it."""
    print("Changed test targets:")
    for target in recommendation.targets:
        print(f"  {target.kind}: {target.name}")
        advice = guidance.advice_for(target)
        if not advice:
            print(f"    {guidance.absent_target_message(target.name, target.kind)}")
            continue
        if advice.sheet:
            print(f"    source: {advice.sheet}")
        if advice.kind:
            print(f"    kind: {advice.kind}")
        print(f"    basis: {advice.basis}")
        print(f"    run in: {' | '.join(advice.suites)}")
        if advice.covered:
            print(
                f"    episodes behind advice: {advice.behind}; "
                f"episodes covered: {advice.covered}"
            )
        if rate := guidance.rates.worst.get(target.name):
            print(f"    rate context: {rate.summary()}")
        if target.kind == CPP_UNIT_TEST:
            _print_cpp_evidence(
                target,
                guidance,
                recommendation.cpp_cells.get(target.name, []),
                recommendation.required_by_project,
                recommendation.unreachable_cpp_cells.get(target.name, []),
            )
    if not recommendation.targets:
        print("  none")

    if not guidance.rates.available:
        print(
            "\nWarning: the failure-rate summary was missing or unreadable, so rate context "
            "and rate-selected C++ variants are omitted; this is not evidence of zero failures."
        )

    if recommendation.ignored_cpp:
        print("\nIgnored production-only C++ changes:")
        for path in recommendation.ignored_cpp:
            print(f"  {path}")

    measured = [
        f"{guidance.accuracy[key]}% at {count} suites"
        for key, count in (("at_3_suites_pct", 3), ("at_5_suites_pct", 5))
        if key in guidance.accuracy
    ]
    if measured:
        # Only the figures the artifact actually carries: a placeholder reads
        # as a broken feature rather than as an absent measurement.
        print(f"\nSheet 17 temporal holdout: {'; '.join(measured)}.")
    else:
        print("\nSheet 17 temporal holdout: unavailable in guidance artifact.")

    unschedulable = set(recommendation.unschedulable_suites)
    print("\nSuggested suites:")
    for suite in recommendation.suites:
        # The suite may well run somewhere in the project; what makes it
        # unschedulable is that no variant either command requests runs it.
        note = "  (no variant this patch requests runs this suite; not scheduled)"
        print(f"  {suite}{note if suite in unschedulable else ''}")
    if not recommendation.suites:
        print("  none; absence of guidance is not evidence of safety")

    print("\nSelected variants:")
    _print_variants(recommendation.variants, recommendation.js_choices)
    if not recommendation.variants and recommendation.suites:
        # "Nothing found" and "already covered" are different answers, and
        # conflating them invites doubt about coverage that does exist. The
        # per-suite reasons live on the choices, which the loop above never
        # reaches when no variant was added.
        already_required = [
            choice
            for choice in recommendation.js_choices
            if choice.variant in recommendation.required
        ]
        if already_required:
            print("  none needed; the suites below already run on required variants")
            for choice in already_required:
                print(f"    {choice.why}")
        else:
            print("  none found in the current Evergreen configuration")

    print("\nEvergreen command:")
    print(shlex.join(recommendation.command))

    if recommendation.nightly_command:
        print(
            f"\nThe evidence below was measured on {NIGHTLY_PROJECT}, so it is routed to "
            "that project. Submit this second patch to exercise it:"
        )
        _print_variants(recommendation.nightly_variants, recommendation.nightly_js_choices)
        print(shlex.join(recommendation.nightly_command))


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    """Parse command line options."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--run-evergreen", action="store_true", help="Submit the patch instead of only printing it."
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    """Suggest and optionally submit an Evergreen patch for the changed tests."""
    args = parse_args(argv)
    repo = REPO
    detected_evergreen = _evergreen_binary()
    evergreen_binary = detected_evergreen or "evergreen"

    changed = changed_files(repo)
    if not changed:
        command = build_evergreen_command(
            [], [], PROJECT, alias=alias_for(PROJECT), evergreen_binary=evergreen_binary
        )
        print("No changed files found; no test-specific guidance was generated.")
        print("\nEvergreen command:")
        print(shlex.join(command))
        if args.run_evergreen:
            if detected_evergreen is None:
                print(
                    "Cannot submit the patch because the evergreen CLI was not found.",
                    file=sys.stderr,
                )
                return 1
            return subprocess.run(command, cwd=repo).returncode
        return 0

    guidance = load_guidance()
    targets, ignored_cpp = collect_targets(changed, load_unit_test_source_map(repo))
    live_required = required_variants(repo, PROJECT)
    master_variant_tasks = variant_tasks(repo, PROJECT)
    nightly_variant_tasks = variant_tasks(repo, NIGHTLY_PROJECT)
    recommendation = recommend(
        targets,
        ignored_cpp,
        guidance,
        master_variant_tasks,
        live_required,
        project=PROJECT,
        nightly_variant_tasks=nightly_variant_tasks,
        suite_to_task=_suite_to_task(repo, PROJECT),
        nightly_suite_to_task=_suite_to_task(repo, NIGHTLY_PROJECT),
        evergreen_binary=evergreen_binary,
        variant_costs=variant_task_counts(repo, PROJECT),
        nightly_variant_costs=variant_task_counts(repo, NIGHTLY_PROJECT),
    )
    _print_report(recommendation, guidance)
    if args.run_evergreen:
        if detected_evergreen is None:
            print(
                "Cannot submit the patch because the evergreen CLI was not found.",
                file=sys.stderr,
            )
            return 1
        returncode = subprocess.run(recommendation.command, cwd=repo).returncode
        if returncode or not recommendation.nightly_command:
            return returncode
        return subprocess.run(recommendation.nightly_command, cwd=repo).returncode
    return 0


if __name__ == "__main__":
    sys.exit(main())
