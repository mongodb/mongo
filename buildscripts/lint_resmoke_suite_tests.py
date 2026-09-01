#!/usr/bin/env python3
"""Enforce parity between Evergreen resmoke tasks and Bazel resmoke_suite_test targets.

The linter enforces the following rules (each function/section below maps to these):
1. Every Evergreen task that runs resmoke has a tag mapping it to a bazel target like `bazel://pkg:test` or `bazel:none`.
2. The tags between the task and bazel target are equal, with some exceptions.
"""

import argparse
import functools
import os
import re
import subprocess
import sys
from abc import ABC, abstractmethod
from dataclasses import dataclass
from pathlib import Path

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from buildscripts.util.buildozer_utils import BuildozerRuleNotFoundError, bd_add
from buildscripts.util.taskname import remove_gen_suffix

REPO_ROOT = Path(os.environ.get("BUILD_WORKSPACE_DIRECTORY") or Path(__file__).resolve().parents[1])
DEFAULT_TASKS_DIR = "etc/evergreen_yml_components/tasks"

# The Evergreen func of a task whose only job is to activate the suite tasks that
# mongo-task-generator generated for it. Such a task runs no tests itself.
GEN_TASK_FUNC = "generate resmoke tasks"

# Evergreen function names that use resmoke to run (or generate tasks that run) a suite.
RESMOKE_FUNCS = frozenset(
    {
        "run tests",
        "run tests with aws credentials",
        GEN_TASK_FUNC,
        "run benchmark tests",
        "run streams tests",
        "run streams tests with mongot",
    }
)

BAZEL_TAG_PREFIX = "bazel:"
BAZEL_NONE = "none"  # the value after the prefix, i.e. the full tag is 'bazel:none'

# Evergreen tags that mark a task as running in the commit-queue / required variants.
CRITICAL_EVERGREEN_TAGS = frozenset({"default", "release_critical"})

TAG_EQUIVALENCES = {
    "default": "ci-default",
    "release_critical": "ci-release-critical",
    "development_critical": "ci-development-critical",
    "experimental": "ci-experimental",
    "fuzzer_deterministic": "ci-fuzzer-deterministic",
}
_EVERGREEN_TO_BAZEL = dict(TAG_EQUIVALENCES)
_BAZEL_TO_EVERGREEN = {bazel: evg for evg, bazel in TAG_EQUIVALENCES.items()}

EVERGREEN_EXEMPT_PATTERNS = (
    "development_critical_single_variant",
    "no_commit_queue",
    "large",
    "requires_large_host*",
    "arm64_tsan_needs_8xlarge",
    "arm64_aubsan_grpc_needs_8xlarge",
    "assigned_to_jira_team_*",  # team ownership metadata
)

# Evergreen 'incompatible_*' tags exclude a task from a variant/platform. On the Bazel side that
# exclusion is expressed with target_compatible_with that resolves to '@platforms//:incompatible'.
# Every other 'incompatible_*' tag has no such mapping and must participate
# in parity like an ordinary tag.
INCOMPATIBLE_TAG_TO_BAZEL_SETTINGS: dict[str, tuple[str, ...]] = {
    "incompatible_windows": ("@platforms//os:windows",),
    "incompatible_mac": ("@platforms//os:macos",),
    "incompatible_ppc": ("@platforms//cpu:ppc64le",),
    "incompatible_s390x": ("@platforms//cpu:s390x",),
    "incompatible_tsan": ("//bazel/config:tsan_enabled",),
    "incompatible_aubsan": ("//bazel/config:asan_enabled", "//bazel/config:ubsan_enabled"),
    "incompatible_amazon_linux2": ("//bazel/platforms:amazon_linux_2",),
    "incompatible_system_allocator": ("//bazel/config:system_allocator_enabled",),
    "incompatible_community": ("//bazel/config:build_enterprise_disabled",),
}

# Evergreen tags that route a task onto a bigger host. They are meaningless on a task whose body is
# GEN_TASK_FUNC: that task only activates its generated tasks through the Evergreen API, and the
# generated tasks take their distro from the 'use_large_distro' var resolved against the build
# variant's 'large_distro_name' expansion, never from the generating task's tags. Tagging the
# generating task this way only moves it onto a larger host, via the 'distros:' of the variant
# selector that the tag matches.
LARGE_HOST_TAG_PATTERNS = ("requires_large_host*",)

BAZEL_EXEMPT_PATTERNS = (
    "no-cache",
    "manual",
    "local",
    "requires_large_host*",
    "requires_xlarge_host*",
    "resmoke_suite_test",
    "resources:*",
    "ci-development-critical-single-variant",  # jsCore is special in that it is tagged default, but also needs to be included in the commit-queue variant by this tag.
)


@dataclass(frozen=True)
class EvergreenTask:
    """A representation of an Evergreen task for the tag rules to inspect."""

    name: str
    source: Path
    ref: str  # "<source>:<line>" of the task's name declaration
    tags: frozenset[str]  # all tags exactly as written in the YAML
    # Whether the task's body is GEN_TASK_FUNC, i.e. it activates tasks generated for it rather than
    # running tests itself. This, not the '_gen' name suffix, identifies a generating task.
    generates_tasks: bool = False
    # Whether that func already asks for the generated tasks to run on the variant's large distro.
    sets_use_large_distro: bool = False

    @classmethod
    def from_dict(cls, task: dict, source: Path) -> "EvergreenTask":
        name = task.get("name", "<unnamed>")
        tags = frozenset(t for t in (task.get("tags") or []) if isinstance(t, str))
        gen_vars = _gen_task_vars(task)
        return cls(
            name=name,
            source=source,
            ref=_task_ref(source, name),
            tags=tags,
            generates_tasks=gen_vars is not None,
            sets_use_large_distro=str(gen_vars.get("use_large_distro", "")).lower() == "true"
            if gen_vars
            else False,
        )

    @property
    def bazel_tags(self) -> frozenset[str]:
        return frozenset(t for t in self.tags if t.startswith(BAZEL_TAG_PREFIX))

    @property
    def labels(self) -> frozenset[str]:
        """The values after the 'bazel:' prefix (target labels, or 'none')."""
        return frozenset(t[len(BAZEL_TAG_PREFIX) :] for t in self.bazel_tags)

    @property
    def non_bazel_tags(self) -> frozenset[str]:
        return self.tags - self.bazel_tags

    @property
    def parity_tags(self) -> frozenset[str]:
        """Non-bazel tags that participate in parity (Evergreen-side exemptions removed)."""
        return frozenset(
            t
            for t in self.non_bazel_tags
            if not _matches(t, EVERGREEN_EXEMPT_PATTERNS)
            and t not in INCOMPATIBLE_TAG_TO_BAZEL_SETTINGS
        )


@dataclass(frozen=True)
class BazelTarget:
    label: str
    ref: str  # "<pkg>/BUILD.bazel:<line>" of the target
    tags: frozenset[str]  # all tags on the target

    # select() keys in target_compatible_with that resolve to '@platforms//:incompatible'
    compatible_exclusions: frozenset[str] | None = None

    @classmethod
    def from_label(
        cls, label: str, tags: set[str], compatible_exclusions: set[str] | None = None
    ) -> "BazelTarget":
        return cls(
            label=label,
            ref=_target_ref(label),
            tags=frozenset(tags),
            compatible_exclusions=(
                None if compatible_exclusions is None else frozenset(compatible_exclusions)
            ),
        )

    @property
    def parity_tags(self) -> frozenset[str]:
        """Tags that participate in parity (Bazel-side exemptions removed)."""
        return frozenset(t for t in self.tags if not _matches(t, BAZEL_EXEMPT_PATTERNS))


@dataclass(frozen=True)
class Violation:
    message: str
    evergreen_tags_to_add: frozenset[str] = frozenset()
    target_label: str | None = None
    bazel_tags_to_add: frozenset[str] = frozenset()
    # Whether --fix reports this violation by printing a copy-ready snippet instead of the message,
    # so that it is not also listed among the issues needing a hand edit.
    has_snippet: bool = False

    @property
    def is_auto_fixable(self) -> bool:
        """Whether the violation carries tags that --fix can add for you."""
        return bool(self.evergreen_tags_to_add or (self.target_label and self.bazel_tags_to_add))


class TagRule(ABC):
    @abstractmethod
    def check(self, task: EvergreenTask, target: BazelTarget | None = None) -> list[Violation]:
        """Return violations for a task.

        Rules in TAG_RULES compare a task against a resolved Bazel `target`. Rules in TASK_RULES
        inspect the task's own 'bazel:*' labels and are called once per task with `target=None`.
        """


class CriticalTasksMustMapToTarget(TagRule):
    """A task tagged 'default' or 'release_critical' must map to a resmoke_suite_test target
    (e.g. 'bazel://jstests/suites/foo:bar').
    """

    @staticmethod
    def applies(task: EvergreenTask) -> bool:
        """Whether the task is critical yet opts out of Bazel coverage with 'bazel:none'."""
        return bool(task.tags & CRITICAL_EVERGREEN_TAGS) and task.labels == {BAZEL_NONE}

    def check(self, task: EvergreenTask, target: BazelTarget | None = None) -> list[Violation]:
        if not self.applies(task):
            return []
        critical = sorted(task.tags & CRITICAL_EVERGREEN_TAGS)
        return [
            Violation(
                f"{task.ref}: task '{task.name}' is tagged {critical} but maps to 'bazel:none'; "
                "default/release_critical tasks must map to a real resmoke_suite_test target "
                "(bazel://pkg:target)",
                has_snippet=True,
            )
        ]


class GeneratingTasksMustNotRequireLargeHost(TagRule):
    """A task that generates tasks must not carry a 'requires_large_host*' tag.

    The tag does not give the generated tests a larger host; it only moves the generating task
    itself, which does nothing but activate the generated tasks over the Evergreen API. Use
    'use_large_distro: "true"' in the vars of the task's GEN_TASK_FUNC to route the tests instead.
    """

    @staticmethod
    def offending_tags(task: EvergreenTask) -> frozenset[str]:
        """The large-host tags on a generating task; empty for any other task."""
        if not task.generates_tasks:
            return frozenset()
        return frozenset(t for t in task.tags if _matches(t, LARGE_HOST_TAG_PATTERNS))

    def check(self, task: EvergreenTask, target: BazelTarget | None = None) -> list[Violation]:
        tags = self.offending_tags(task)
        if not tags:
            return []
        remedy = (
            "it already sets 'use_large_distro', so the tag is redundant and should be removed"
            if task.sets_use_large_distro
            else "to give the generated tests a larger host, set 'use_large_distro: \"true\"' in "
            f"the vars of the task's '{GEN_TASK_FUNC}' func instead"
        )
        return [
            Violation(
                f"{task.ref}: task '{task.name}' is tagged {sorted(tags)}, which a task running "
                f"'{GEN_TASK_FUNC}' must not use: the tag only moves the generating task itself, "
                f"which merely activates the generated tasks over the Evergreen API; {remedy}."
            )
        ]


class EvergreenTagsMustBeOnTarget(TagRule):
    def check(self, task: EvergreenTask, target: BazelTarget) -> list[Violation]:
        missing = sorted(t for t in task.parity_tags if _to_bazel_spelling(t) not in target.tags)
        if not missing:
            return []
        return [
            Violation(
                f"{task.ref}: task '{task.name}': tag(s) present in Evergreen but absent from"
                f" Bazel target {target.label} ({target.ref}): {missing}"
                f" — run with --fix to add them to {target.ref}",
                target_label=target.label,
                bazel_tags_to_add=frozenset(_to_bazel_spelling(t) for t in missing),
            )
        ]


class TargetTagsMustBeOnEvergreen(TagRule):
    def check(self, task: EvergreenTask, target: BazelTarget) -> list[Violation]:
        missing = sorted(
            _to_evergreen_spelling(t)
            for t in target.parity_tags
            if _to_evergreen_spelling(t) not in task.non_bazel_tags
        )
        if not missing:
            return []
        return [
            Violation(
                f"{task.ref}: task '{task.name}': tag(s) present in Bazel target {target.label}"
                f" ({target.ref}) but absent from Evergreen task: {missing}",
                evergreen_tags_to_add=frozenset(missing),
            )
        ]


class IncompatibleTagsMustExcludeInBazel(TagRule):
    """An Evergreen 'incompatible_*' tag with a Bazel mapping must be mirrored by the target's
    target_compatible_with. For each such tag, every setting in INCOMPATIBLE_TAG_TO_BAZEL_SETTINGS
    must appear as a select() key resolving to '@platforms//:incompatible'.
    """

    def check(self, task: EvergreenTask, target: BazelTarget) -> list[Violation]:
        if target.compatible_exclusions is None:
            return []
        violations: list[Violation] = []
        for tag in sorted(task.non_bazel_tags):
            settings = INCOMPATIBLE_TAG_TO_BAZEL_SETTINGS.get(tag)
            if not settings:
                continue
            missing = [s for s in settings if s not in target.compatible_exclusions]
            if missing:
                violations.append(
                    Violation(
                        f"{task.ref}: task '{task.name}' is tagged '{tag}' but Bazel target"
                        f" {target.label} ({target.ref}) does not exclude {missing} in"
                        " target_compatible_with; add a select() entry mapping each to"
                        " ['@platforms//:incompatible'] (not auto-fixable)"
                    )
                )
        return violations


# Rules that compare a task against a resolved Bazel target.
TAG_RULES: tuple[TagRule, ...] = (
    IncompatibleTagsMustExcludeInBazel(),
    EvergreenTagsMustBeOnTarget(),
    TargetTagsMustBeOnEvergreen(),
)

# Rules that inspect a task's own 'bazel:*' labels (no target needed), run once per task.
TASK_RULES: tuple[TagRule, ...] = (
    CriticalTasksMustMapToTarget(),
    GeneratingTasksMustNotRequireLargeHost(),
)


class TaskResult:
    """Outcome of checking one Evergreen task: violations plus the additions that would fix tag issues."""

    def __init__(self, task: EvergreenTask):
        self.task = task
        self.violations: list[str] = []
        # Violations that --fix cannot resolve, so it must report them rather than pass silently.
        self.unfixable: list[str] = []
        self.needs_bazel_tag = False
        self.needs_real_target = False  # critical task that maps to 'bazel:none'
        self.evergreen_tags_to_add: set[str] = set()
        self.target_tags_to_add: dict[str, set[str]] = {}

    def add(self, violation: Violation) -> None:
        """Record a violation, tracking whether --fix will be able to resolve it."""
        self.violations.append(violation.message)
        if not violation.is_auto_fixable and not violation.has_snippet:
            self.unfixable.append(violation.message)


def check_task(
    task: dict,
    source: Path,
    label_to_tags: dict[str, set[str]],
    label_to_exclusions: dict[str, set[str]] | None = None,
) -> TaskResult:
    """Apply the tag rules to one task: Rule 1 / resolution preconditions, then TAG_RULES.

    `label_to_exclusions` maps each target to its target_compatible_with exclusion settings.
    """
    evg = EvergreenTask.from_dict(task, source)
    result = TaskResult(evg)

    # Task-level rules: inspect the task's own name and tags (e.g. 'bazel:none' coverage
    # requirements). These run before the precondition below so that a task missing its 'bazel:*'
    # tag still reports every other problem it has.
    for rule in TASK_RULES:
        for violation in rule.check(evg):
            result.add(violation)
    result.needs_real_target = CriticalTasksMustMapToTarget.applies(evg)

    # Precondition: the task has at least one bazel:* tag.
    if not evg.bazel_tags:
        result.violations.append(f"{evg.ref}: task '{evg.name}' has no 'bazel:*' tag")
        result.needs_bazel_tag = True
        return result

    # Precondition: bazel:none must be used alone.
    if BAZEL_NONE in evg.labels:
        if evg.labels != {BAZEL_NONE}:
            result.add(
                Violation(
                    f"{evg.ref}: task '{evg.name}' mixes 'bazel:none' with label tags: "
                    f"{sorted(evg.bazel_tags)}"
                )
            )
        return result

    for label in sorted(evg.labels):
        # Precondition: the label must resolve to a real resmoke_suite_test target.
        if label not in label_to_tags:
            result.add(
                Violation(f"{evg.ref}: task '{evg.name}' references unknown Bazel target '{label}'")
            )
            continue

        exclusions = None if label_to_exclusions is None else label_to_exclusions.get(label, set())
        target = BazelTarget.from_label(label, label_to_tags[label], exclusions)
        for rule in TAG_RULES:
            for violation in rule.check(evg, target):
                result.add(violation)
                result.evergreen_tags_to_add |= violation.evergreen_tags_to_add
                if violation.target_label and violation.bazel_tags_to_add:
                    result.target_tags_to_add.setdefault(violation.target_label, set()).update(
                        violation.bazel_tags_to_add
                    )

    return result


def _gen_task_vars(task: dict) -> dict | None:
    """Return the vars of the task's GEN_TASK_FUNC command, or None if it does not run that func."""
    for command in task.get("commands") or []:
        if isinstance(command, dict) and command.get("func") == GEN_TASK_FUNC:
            variables = command.get("vars")
            return variables if isinstance(variables, dict) else {}
    return None


def _matches(tag: str, patterns: tuple[str, ...]) -> bool:
    """Return whether tag matches any pattern (trailing '*' = prefix match, else exact)."""
    for pattern in patterns:
        if pattern.endswith("*"):
            if tag.startswith(pattern[:-1]):
                return True
        elif tag == pattern:
            return True
    return False


def _to_bazel_spelling(evergreen_tag: str) -> str:
    """Map an Evergreen tag to its Bazel spelling; unchanged if no equivalence."""
    return _EVERGREEN_TO_BAZEL.get(evergreen_tag, evergreen_tag)


def _to_evergreen_spelling(bazel_tag: str) -> str:
    """Map a Bazel tag to its Evergreen spelling; unchanged if no equivalence."""
    return _BAZEL_TO_EVERGREEN.get(bazel_tag, bazel_tag)


def _is_resmoke_task(task: dict) -> bool:
    """Whether a task invokes runs resmoke."""
    for command in task.get("commands", []) or []:
        if isinstance(command, dict) and command.get("func") in RESMOKE_FUNCS:
            return True
    return False


def load_resmoke_tasks(tasks_dir: Path) -> list[tuple[Path, dict]]:
    """Return (absolute_yaml_path, task_dict) for every resmoke task."""
    out: list[tuple[Path, dict]] = []
    for yaml_file in sorted(tasks_dir.rglob("*.yml")):
        data = yaml.safe_load(yaml_file.read_text(encoding="utf-8"))
        if not isinstance(data, dict) or "tasks" not in data:
            continue
        for task in data["tasks"]:
            if isinstance(task, dict) and "name" in task and _is_resmoke_task(task):
                out.append((yaml_file, task))
    return out


# A select() key mapping to exactly ['@platforms//:incompatible'], e.g.
#   "@platforms//os:macos": ["@platforms//:incompatible"]
# as printed on the single-line `target_compatible_with = select({...})` of `--output=build`.
_INCOMPATIBLE_SELECT_KEY_RE = re.compile(r'"([^"]+)":\s*\["@platforms//:incompatible"\]')

# A quoted string entry, used to pull the values out of a single-line `tags = [...]` list.
_QUOTED_RE = re.compile(r'"([^"]*)"')


def _parse_targets_build(build_text: str) -> tuple[dict[str, set[str]], dict[str, set[str]]]:
    """Parse `bazel query --output=build` into ({label: tags}, {label: exclusion keys})."""
    label_to_tags: dict[str, set[str]] = {}
    label_to_exclusions: dict[str, set[str]] = {}
    name: str | None = None
    package: str | None = None
    tags: set[str] = set()
    compatible_line: str | None = None

    def flush() -> None:
        if name is not None and package is not None and "resmoke_suite_test" in tags:
            label = f"//{package}:{name}"
            label_to_tags[label] = set(tags)
            label_to_exclusions[label] = set(
                _INCOMPATIBLE_SELECT_KEY_RE.findall(compatible_line or "")
            )

    for line in build_text.splitlines():
        # Each rule stanza begins with an unindented `<rule_class>(` line.
        if re.match(r"^[A-Za-z_][A-Za-z0-9_]*\(\s*$", line):
            flush()
            name = package = compatible_line = None
            tags = set()
            continue
        m = re.match(r'^  name = "([^"]+)"', line)
        if m:
            name = m.group(1)
            continue
        m = re.match(r'^  generator_location = "(.+?)/BUILD\.bazel:', line)
        if m:
            package = m.group(1)
            continue
        m = re.match(r"^  tags = \[(.*)\],\s*$", line)
        if m:
            tags = set(_QUOTED_RE.findall(m.group(1)))
            continue
        m = re.match(r"^  target_compatible_with = (.*)$", line)
        if m:
            compatible_line = m.group(1)
    flush()
    return label_to_tags, label_to_exclusions


def query_targets(bazel_bin: str) -> tuple[dict[str, set[str]], dict[str, set[str]]]:
    """Query every resmoke_suite_test target's tags and target_compatible_with exclusions."""
    result = subprocess.run(
        [
            bazel_bin,
            "query",
            "attr('tags','resmoke_suite_test',//...)",
            "--output=build",
            "--keep_going",
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    _warn_on_query_errors(result.returncode, result.stderr)
    if not result.stdout.strip():
        print(f"ERROR: bazel query returned no targets.\n{result.stderr}", file=sys.stderr)
    return _parse_targets_build(result.stdout)


def _warn_on_query_errors(returncode: int, stderr: str) -> None:
    """Surface bazel query load errors.

    With --keep_going a BUILD that fails to load is skipped (returncode 3) and its targets are
    silently absent from the results, which then surface as misleading 'references unknown Bazel
    target' violations. Print the load errors so the real cause (a broken BUILD) is obvious.
    """
    if returncode == 0:
        return
    errors = "\n".join(line for line in stderr.splitlines() if "ERROR" in line) or stderr.strip()
    print(
        "WARNING: `bazel query` did not load every package; some resmoke_suite_test targets may be "
        "missing, which can surface as false 'unknown target' violations below. Fix the broken "
        f"BUILD file(s):\n{errors}",
        file=sys.stderr,
    )


@functools.lru_cache(maxsize=None)
def _read_lines(path_str: str) -> tuple[str, ...]:
    try:
        return tuple(Path(path_str).read_text(encoding="utf-8").splitlines())
    except OSError:
        return ()


def _task_ref(source: Path, task_name: str) -> str:
    """'<source>:<line>' of the YAML task's own 'name:' declaration"""
    pattern = re.compile(rf"^\s{{0,4}}-?\s*name:\s+{re.escape(task_name)}\s*$")
    for i, line in enumerate(_read_lines(str(source)), 1):
        if pattern.match(line):
            return f"{source}:{i}"
    return str(source)


def _target_ref(label: str) -> str:
    """'<pkg>/BUILD.bazel:<line>' for a '//pkg:name' label, best effort.

    Macro/comprehension-generated targets may have no literal name="..." line, in which case just
    the BUILD.bazel path is returned (still clickable to the right file).
    """
    if not label.startswith("//") or ":" not in label:
        return label
    package, name = label[len("//") :].split(":", 1)
    build_file = REPO_ROOT / package / "BUILD.bazel"
    if not build_file.exists():
        return label
    pattern = re.compile(rf'\bname\s*=\s*"{re.escape(name)}"')
    for i, line in enumerate(_read_lines(str(build_file)), 1):
        if pattern.search(line):
            return f"{package}/BUILD.bazel:{i}"
    return f"{package}/BUILD.bazel"


def resmoke_suite_test_snippet(task: EvergreenTask) -> str:
    """Emit a copy-ready resmoke_suite_test for a task that is missing a 'bazel:*' tag."""
    suite = remove_gen_suffix(task.name)
    bazel_tags = sorted(_to_bazel_spelling(t) for t in task.parity_tags)
    tag_lines = "".join(f'        "{t}",\n' for t in bazel_tags)
    return (
        f"# Add to the appropriate jstests/suites/<dir>/BUILD.bazel, then tag the task with\n"
        f"# 'bazel://jstests/suites/<dir>:{suite}' (or 'bazel:none' if no suite applies):\n"
        f"resmoke_suite_test(\n"
        f'    name = "{suite}",\n'
        f'    config = "//buildscripts/resmokeconfig:suites/{suite}.yml",\n'
        f"    tags = [\n"
        f"{tag_lines}"
        f"    ],\n"
        f"    deps = [\n"
        f'        "//src/mongo/db:mongod",\n'
        f'        "//src/mongo/shell:mongo",\n'
        f"    ],\n"
        f")"
    )


_TOP_LEVEL_KEY_RE = re.compile(r"^\S")


def _tasks_section_bounds(lines: list[str]) -> tuple[int, int] | None:
    start = None
    for i, line in enumerate(lines):
        if re.match(r"^tasks:\s*$", line):
            start = i + 1
            break
    if start is None:
        return None
    end = len(lines)
    for i in range(start, len(lines)):
        if _TOP_LEVEL_KEY_RE.match(lines[i]):
            end = i
            break
    return start, end


def _item_indent(lines: list[str], start: int, end: int) -> str | None:
    for i in range(start, end):
        m = re.match(r"^(\s*)-\s", lines[i])
        if m:
            return m.group(1)
    return None


def _iter_task_items(lines: list[str], start: int, end: int, indent: str):
    item_re = re.compile(rf"^{re.escape(indent)}-\s")
    starts = [i for i in range(start, end) if item_re.match(lines[i])]
    for idx, s in enumerate(starts):
        e = starts[idx + 1] if idx + 1 < len(starts) else end
        yield s, e


def _task_name_in_item(lines: list[str], s: int, e: int, indent: str) -> str | None:
    body_indent = indent + "  "
    dash_name = re.match(rf"^{re.escape(indent)}-\s+name:\s+(\S+)\s*$", lines[s])
    if dash_name:
        return dash_name.group(1)
    for i in range(s, e):
        m = re.match(rf"^{re.escape(body_indent)}name:\s+(\S+)\s*$", lines[i])
        if m:
            return m.group(1)
    return None


def _find_tags_block(lines: list[str], s: int, e: int, indent: str) -> tuple[int, int] | None:
    body_indent = indent + "  "
    for i in range(s, e):
        if re.match(rf"^{re.escape(body_indent)}tags:", lines[i]):
            if "[" in lines[i] and "]" in lines[i]:
                return i, i + 1
            for j in range(i + 1, e):
                if "]" in lines[j]:
                    return i, j + 1
            return i, i + 1
    return None


def _insert_into_tags_block(block: list[str], new_tags: list[str], body_indent: str) -> list[str]:
    """Splice new tag entries into a tags block, preserving every existing line and its comments."""
    first = block[0]
    if "[" in first and "]" in first:
        # Single-line: 'tags: [...]' — insert before the final ']'.
        close = first.rfind("]")
        head, tail = first[:close], first[close:]
        sep = "" if head.rstrip().endswith("[") else ", "
        return [head.rstrip() + sep + ", ".join(f'"{t}"' for t in new_tags) + tail]
    # Multi-line: keep all existing lines, insert new entries before the closing ']' line,
    # matching the indentation of the existing entries.
    entry_indent = next(
        (
            re.match(r"^(\s*)", line).group(1)
            for line in block[1:-1]
            if line.lstrip().startswith('"')
        ),
        body_indent + "    ",
    )
    new_entries = [f'{entry_indent}"{t}",' for t in new_tags]
    return block[:-1] + new_entries + [block[-1]]


def add_tags_to_yaml(path: Path, additions: dict[str, set[str]]) -> list[str]:
    """Add the given tags to the named tasks in a YAML file. Returns notes for changes made."""
    lines = path.read_text(encoding="utf-8").splitlines()
    bounds = _tasks_section_bounds(lines)
    if bounds is None:
        return []
    start, end = bounds
    indent = _item_indent(lines, start, end)
    if indent is None:
        return []
    body_indent = indent + "  "

    edits: list[tuple[int, int, list[str], str, list[str]]] = []
    for s, e in _iter_task_items(lines, start, end, indent):
        name = _task_name_in_item(lines, s, e, indent)
        if name is None or name not in additions:
            continue
        tags_range = _find_tags_block(lines, s, e, indent)
        if tags_range is None:
            continue
        ts, te = tags_range
        existing = set(re.findall(r'"([^"]*)"', "\n".join(lines[ts:te])))
        new_tags = [t for t in sorted(additions[name]) if t not in existing]
        if not new_tags:
            continue
        new_block = _insert_into_tags_block(lines[ts:te], new_tags, body_indent)
        edits.append((ts, te, new_block, name, new_tags))

    notes = []
    for ts, te, new_block, name, new_tags in sorted(edits, key=lambda x: x[0], reverse=True):
        lines[ts:te] = new_block
        notes.append(f"{path}: added {new_tags} to task '{name}'")
    if edits:
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return notes


def add_tags_to_targets(target_additions: dict[str, set[str]]) -> tuple[list[str], list[str]]:
    """Add tags to Bazel targets via buildozer."""
    fixed: list[str] = []
    manual: list[str] = []
    original_cwd = os.getcwd()
    os.chdir(REPO_ROOT)
    try:
        for label in sorted(target_additions):
            tags = sorted(target_additions[label])
            try:
                bd_add([label], "tags", tags)
                fixed.append(f"{label}: added {tags}")
            except BuildozerRuleNotFoundError:
                manual.append(
                    f"{label}: add {tags} to its BUILD.bazel manually "
                    "(buildozer can't edit a macro/comprehension-generated target)"
                )
            except Exception:
                manual.append(
                    f"{label}: add {tags} to its BUILD.bazel manually (buildozer failed; see output)"
                )
    finally:
        os.chdir(original_cwd)
    return fixed, manual


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--target-info-build",
        default=None,
        help="File with `bazel query --output=build` output (skips an internal bazel query). ",
    )
    parser.add_argument("--fix", action="store_true", help="Reconcile tag-only issues in place.")
    args = parser.parse_args()

    if args.target_info_build:
        label_to_tags, label_to_exclusions = _parse_targets_build(
            Path(args.target_info_build).read_text()
        )
    else:
        label_to_tags, label_to_exclusions = query_targets("bazel")

    tasks = load_resmoke_tasks(REPO_ROOT / DEFAULT_TASKS_DIR)

    results = [
        check_task(task, source, label_to_tags, label_to_exclusions) for source, task in tasks
    ]
    violations = [v for r in results for v in r.violations]

    missing_tag = [r.task for r in results if r.needs_bazel_tag]
    needs_real_target = [r.task for r in results if r.needs_real_target]

    def print_missing_tag_snippets() -> None:
        if missing_tag:
            print(
                f"\n{len(missing_tag)} task(s) are missing a 'bazel:*' tag (not auto-fixable). "
                "Create the suite target (snippet below) and tag the task, or add 'bazel:none':\n"
            )
            for task in missing_tag:
                print(f"# --- {task.name} ---")
                print(resmoke_suite_test_snippet(task))
                print()
        if needs_real_target:
            print(
                f"\n{len(needs_real_target)} default/release_critical task(s) map to 'bazel:none' "
                "but must map to a real target (not auto-fixable). Create the suite target (snippet "
                "below) and tag the task with its 'bazel://...' label:\n"
            )
            for task in needs_real_target:
                print(f"# --- {task.name} ---")
                print(resmoke_suite_test_snippet(task))
                print()

    if not args.fix:
        for v in violations:
            print(f"  {v}")
        if missing_tag or needs_real_target:
            print_missing_tag_snippets()
        if violations:
            print(
                f"\n{len(violations)} parity violation(s). Run "
                "`bazel run lint --fix` to "
                "reconcile tag-only issues."
            )
            return 1
        print(f"All {len(tasks)} resmoke tasks pass Bazel tag parity checks.")
        return 0

    # Add target tags to the Evergreen task YAML.
    evg_by_file: dict[Path, dict[str, set[str]]] = {}
    for r in results:
        if r.evergreen_tags_to_add:
            evg_by_file.setdefault(r.task.source, {})[r.task.name] = set(r.evergreen_tags_to_add)

    notes: list[str] = []
    for src, adds in evg_by_file.items():
        notes.extend(add_tags_to_yaml(src, adds))

    # Add missing tags to the Bazel targets' BUILD.bazel.
    target_additions: dict[str, set[str]] = {}
    for r in results:
        for label, tags in r.target_tags_to_add.items():
            target_additions.setdefault(label, set()).update(tags)

    target_fixed: list[str] = []
    manual: list[str] = []
    if target_additions:
        target_fixed, manual = add_tags_to_targets(target_additions)

    fixed = notes + target_fixed
    print(f"\nFixed {len(fixed)} tag issue(s):")
    for n in fixed:
        print(f"  {n}")

    # Report everything --fix could not resolve. Every violation that carries no tags for --fix to
    # add is listed here, so a new rule cannot silently pass under --fix.
    not_auto_fixable = [v for r in results for v in r.unfixable]
    if manual:
        print(f"\n{len(manual)} Bazel target tag issue(s) require manual fixing:")
        for m in manual:
            print(f"  {m}")
    if not_auto_fixable:
        print(f"\n{len(not_auto_fixable)} issue(s) require manual fixing:")
        for v in not_auto_fixable:
            print(f"  {v}")
    if missing_tag or needs_real_target:
        print_missing_tag_snippets()

    # The run fails if anything remains unresolved after fixing.
    return 1 if (manual or not_auto_fixable or missing_tag or needs_real_target) else 0


if __name__ == "__main__":
    sys.exit(main())
