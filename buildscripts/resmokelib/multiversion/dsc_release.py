"""Resolve the old-side DSC release for the disagg multiversion suites.

Instead of hard-coding a release in the variant YAML, resolve automatically
and verifiably:

> the newest release whose DSC binary is published in the configured feed, and
> strictly older than the version under test

DSC release trains are branches ``dsc-release-N`` in 10gen/mongo whose builds
are tagged with a continuing rc counter (``dsc-release-5`` -> ``r9.0.0-rc1013``
.. ``r9.0.0-rc1020``, ``dsc-release-6`` -> ``r9.1.0-rc1021``). A tag existing is
not sufficient -- a DSC binary is not necessarily published for every tag -- so
every candidate is checked for a published binary for the requested
platform/architecture/edition, stepping down (across train boundaries) until
one qualifies.

Contract:

* stdout carries exactly one line: the resolved version string. The caller
  (evergreen/multiversion_selection.sh) appends ``=last-patch`` and hands
  the result to db-contrib-tool. Every diagnostic goes to stderr.
* Exit codes: 0 = resolved; 1 = nothing resolvable (stdout stays empty; the
  caller decides fail vs. skip); 2 = usage/config error.
* All network I/O is behind :class:`VersionSource` implementations -- the same
  injected-client pattern as ``MultiversionService.last_patch_resolver`` --
  which is what makes the policy unit-testable with fake sources, no network.
  The implementations live in
  :mod:`buildscripts.resmokelib.multiversion.dsc_release_sources`.
* An explicit override (``--override``) bypasses discovery entirely and is
  echoed verbatim. The ``multiversion_dsc_release`` expansion is the *shell's*
  emergency pin -- the resolver never reads it, so the shell can compare
  discovery against the pin.

Discovery sources, in ``--source auto`` order (documented in detail in the
sources module):

* ``feed`` (authoritative): the atlas release feed's JSON manifest, which
  directly answers both enumeration and publication;
* ``evg``: git-tag candidates with an Evergreen build-status publication
  proxy -- needs ``viewTasks`` on the DSC projects, which task credentials do
  not have, so it currently 404s everywhere;
* ``tags``: git-tag candidates with the feed's existence probe as the
  publication check.

This script runs inside ``activate_venv`` with a full checkout (see the
"do multiversion selection" function in
etc/evergreen_yml_components/definitions.yml), so it may use repo-external
dependencies (structlog, packaging) and read files from the checkout.
"""

from __future__ import annotations

import argparse
import logging
import os
import re
import sys
from dataclasses import dataclass
from enum import Enum
from typing import Iterator, Mapping, Optional, Sequence

import structlog
from packaging.version import InvalidVersion, Version

from buildscripts.resmokelib.multiversion.dsc_release_base import (
    ConfigError,
    DscBinaryQuery,
    VersionSource,
)
from buildscripts.resmokelib.multiversion.dsc_release_sources import (
    DSC_EVG_PROJECTS,
    EvgVersionSource,
    FeedSource,
    TagSource,
)

LOGGER = structlog.get_logger(__name__)

# The version the current branch is targeting, e.g. "9.1.0" -- same file and
# parsing as evergreen/fcv_test_git_tag.sh.
MONGO_VERSION_FILE = ".bazelrc.target_mongo_version"
MONGO_VERSION_LINE_RE = re.compile(r"MONGO_VERSION=(\S+)")

# Expansions injected into the task environment as plain env vars by
# etc/evergreen_yml_components/definitions.yml ("do multiversion selection")
# and the variant YAML.
PLATFORM_ENV_VAR = "multiversion_platform"
ARCHITECTURE_ENV_VAR = "multiversion_architecture"
EDITION_ENV_VAR = "multiversion_edition"
OVERRIDE_ENV_VAR = "multiversion_dsc_release"

EXIT_RESOLVED = 0
EXIT_NOTHING_RESOLVABLE = 1
EXIT_USAGE_ERROR = 2


@dataclass(frozen=True)
class ResolverConfig:
    """Inputs gathered from the task environment (expansions are plain env vars).

    Deliberately excludes the version pin: the shell owns
    ``multiversion_dsc_release`` (the emergency pin) and the resolver always
    discovers, so the shell can compare discovery against the pin.
    """

    query: DscBinaryQuery

    @classmethod
    def from_env(cls, env: Mapping[str, str]) -> ResolverConfig:
        """Build the config from the task's env, erroring on required-but-missing vars."""
        missing = [
            name
            for name in (PLATFORM_ENV_VAR, ARCHITECTURE_ENV_VAR, EDITION_ENV_VAR)
            if not env.get(name, "").strip()
        ]
        if missing:
            raise ConfigError(f"missing required environment variables: {', '.join(missing)}")
        return cls(
            query=DscBinaryQuery(
                platform=env[PLATFORM_ENV_VAR].strip(),
                architecture=env[ARCHITECTURE_ENV_VAR].strip(),
                edition=env[EDITION_ENV_VAR].strip(),
            ),
        )


def read_version_under_test(path: str = MONGO_VERSION_FILE) -> str:
    """Read the version the current branch targets from ``.bazelrc.target_mongo_version``.

    Same source and parsing as evergreen/fcv_test_git_tag.sh. Raises
    :class:`ConfigError` when the file or the ``MONGO_VERSION=`` line is missing.
    """
    try:
        with open(path, "r", encoding="utf8") as mongo_version_file:
            content = mongo_version_file.read()
    except OSError as exc:
        raise ConfigError(f"could not read {path}: {exc}") from exc
    match = MONGO_VERSION_LINE_RE.search(content)
    if not match:
        raise ConfigError(f"no MONGO_VERSION= line found in {path}")
    return match.group(1)


class _Eligibility(Enum):
    """Whether a candidate may be resolved -- explicit instead of True/False/None.

    UNPARSEABLE and TOO_NEW candidates are still reported (and shown by
    --print-candidates) but are never resolved. The value is the form
    --print-candidates renders, so the printed vocabulary has one definition.
    """

    ELIGIBLE = "True"
    UNPARSEABLE = "unparseable"
    TOO_NEW = "False"


class _ProbeAnswer(Enum):
    """The probe's answer to "does this candidate have a published DSC binary?".

    PUBLISHED / UNPUBLISHED are the probe's verdicts. CANNOT_DETERMINE means
    the probe cannot verify any candidate of this source, so resolution moves
    to the next source.
    """

    PUBLISHED = "published"
    UNPUBLISHED = "unpublished"
    CANNOT_DETERMINE = "cannot-determine"


@dataclass(frozen=True)
class _Examined:
    """One candidate of one source, as examined by the shared traversal.

    ``eligibility`` is decided here; probing (``has_published_binary``) is the
    consumers' concern: resolve() probes only eligible candidates,
    --print-candidates reports every probe.
    """

    source: VersionSource
    version: str
    eligibility: _Eligibility


def _examine_candidates(
    source: VersionSource, candidates: Sequence[str], limit: Version, version_under_test: str
) -> Iterator[_Examined]:
    """Examine each candidate: parseable, and strictly older than the version under test."""
    for version in candidates:
        try:
            parsed = Version(version)
        except InvalidVersion:
            LOGGER.warning(
                "Ignoring candidate with unparseable version",
                source=source.name,
                version=version,
            )
            eligibility = _Eligibility.UNPARSEABLE
        else:
            if parsed >= limit:
                LOGGER.info(
                    "Candidate is not older than the version under test; excluded",
                    source=source.name,
                    version=version,
                    version_under_test=version_under_test,
                )
                eligibility = _Eligibility.TOO_NEW
            else:
                eligibility = _Eligibility.ELIGIBLE
        yield _Examined(source=source, version=version, eligibility=eligibility)


def _examined_sources(
    sources: Sequence[VersionSource], limit: Version, version_under_test: str
) -> Iterator[tuple[VersionSource, Iterator[_Examined]]]:
    """Walk the discovery chain source by source, examining every candidate.

    Sources that cannot be examined -- not implemented, or failed to enumerate
    -- are logged here (once) and skipped: they yield nothing. Lazily:
    --print-candidates consumes everything, resolve() short-circuits.
    """
    for source in sources:
        if not source.implemented:
            LOGGER.warning("Discovery source is not implemented yet; skipping", source=source.name)
            continue
        try:
            candidates = source.candidate_versions()
        except Exception as exc:
            LOGGER.warning(
                "Discovery source failed; falling through to the next source",
                source=source.name,
                error=str(exc),
            )
            continue
        LOGGER.debug("Enumerated candidates", source=source.name, candidates=candidates)
        yield source, _examine_candidates(source, candidates, limit, version_under_test)


def _published(source: VersionSource, version: str) -> _ProbeAnswer:
    """Ask the source whether ``version`` has a published DSC binary.

    ConfigError propagates: a configuration error (e.g. an unknown platform)
    would fail for every source and candidate alike and must surface as the
    usage error it is instead of being swallowed as "unpublished".
    """
    try:
        return (
            _ProbeAnswer.PUBLISHED
            if source.has_published_binary(version)
            else _ProbeAnswer.UNPUBLISHED
        )
    except NotImplementedError as exc:
        LOGGER.warning(
            "Availability probe not implemented; source cannot resolve anything",
            source=source.name,
            error=str(exc),
        )
        return _ProbeAnswer.CANNOT_DETERMINE
    except ConfigError:
        raise
    except Exception as exc:
        LOGGER.warning(
            "Availability probe failed; stepping down",
            source=source.name,
            version=version,
            error=str(exc),
        )
        return _ProbeAnswer.UNPUBLISHED


def resolve(sources: Sequence[VersionSource], version_under_test: str) -> Optional[str]:
    """Return the newest published candidate strictly older than the version under test.

    Steps down candidate by candidate, newest first, across train boundaries,
    until one qualifies; ``None`` when nothing does. This is the testable
    policy core:

    * candidates are consumed newest-first in the order the source returns
      them -- ordering is the source's responsibility (for git tags, git's
      versioncmp, the same ordering the last-patch machinery trusts);
    * unimplemented sources are skipped (loudly);
    * a source that fails to enumerate (missing credentials, no git repository,
      ...) falls through to the next source -- a discovery route being
      unavailable must not take the whole task down;
    * a candidate at or newer than ``version_under_test`` is excluded, so a
      build is never tested against itself or something newer;
    * an availability probe failure steps down to the next candidate;
      a probe that is not implemented moves to the next source, since no
      candidate of that source can be verified.

    The traversal itself is shared with --print-candidates, which reports
    every candidate instead of stopping at the first published one.
    """
    limit = Version(version_under_test)
    for source, candidates in _examined_sources(sources, limit, version_under_test):
        for record in candidates:
            if record.eligibility is not _Eligibility.ELIGIBLE:
                continue
            answer = _published(source, record.version)
            if answer is _ProbeAnswer.CANNOT_DETERMINE:
                # The probe cannot verify any candidate of this source: stop
                # probing and move to the next source.
                break
            if answer is _ProbeAnswer.PUBLISHED:
                LOGGER.info("Resolved DSC release", source=source.name, version=record.version)
                return record.version
            LOGGER.info(
                "Candidate has no published binary; stepping down",
                source=source.name,
                version=record.version,
            )
        LOGGER.info("Source exhausted without a qualifying candidate", source=source.name)
    return None


def _build_sources(
    choice: str, config: ResolverConfig, evg_projects: Optional[Sequence[str]] = None
) -> list[VersionSource]:
    """Build the discovery chain for ``choice``; auto tries feed, then EVG, then tags."""
    feed = FeedSource(config.query)
    projects = evg_projects or DSC_EVG_PROJECTS
    if choice == "auto":
        return [feed, EvgVersionSource(config.query, projects=projects), TagSource(probe=feed)]
    if choice == "feed":
        return [feed]
    if choice == "evg":
        return [EvgVersionSource(config.query, projects=projects)]
    if choice == "tags":
        return [TagSource(probe=feed)]
    raise ConfigError(f"unknown --source choice: {choice}")


def _parse_args(argv: Optional[Sequence[str]]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Resolve the newest published DSC release for multiversion testing."
        " Prints exactly one line -- the version -- on stdout.",
    )
    parser.add_argument(
        "--source",
        choices=("auto", "feed", "evg", "tags"),
        default="auto",
        help="discovery route; auto tries feed, then EVG, then git tags + feed probe",
    )
    parser.add_argument(
        "--override",
        default=None,
        help="skip discovery and emit this version verbatim; the shell passes the"
        f" {OVERRIDE_ENV_VAR} expansion here when it wants to pin",
    )
    parser.add_argument(
        "--version-under-test",
        default=None,
        help=f"version the current build targets; defaults to MONGO_VERSION in {MONGO_VERSION_FILE}",
    )
    parser.add_argument(
        "--evg-project",
        action="append",
        default=None,
        help="Evergreen project to check for DSC builds; repeatable. Defaults to the known"
        " dsc-release trains",
    )
    parser.add_argument("--debug", action="store_true", help="decision trace on stderr")
    parser.add_argument(
        "--print-candidates",
        action="store_true",
        help="print every source's candidates with publication status and exit;"
        " for incident triage",
    )
    return parser.parse_args(argv)


def _configure_logging(debug: bool) -> None:
    """Send all diagnostics to stderr -- stdout must carry only the result."""
    structlog.configure(
        cache_logger_on_first_use=False,
        logger_factory=structlog.PrintLoggerFactory(file=sys.stderr),
        processors=[
            structlog.processors.add_log_level,
            structlog.processors.KeyValueRenderer(key_order=["event"]),
        ],
        wrapper_class=structlog.make_filtering_bound_logger(
            logging.DEBUG if debug else logging.INFO
        ),
    )


def _print_candidates(sources: Sequence[VersionSource], version_under_test: str) -> int:
    """List each source's candidates with publication status (incident triage).

    Consumes the same traversal as resolve(), but reports every candidate
    (including the ineligible ones) and renders probe failures instead of
    acting on them. Sources that cannot be examined are logged by the
    traversal, not printed. The exit code is EXIT_RESOLVED only when at least
    one probe actually answered: an invocation that learned nothing -- nothing
    enumerated, or every probe failed -- exits EXIT_NOTHING_RESOLVABLE.
    """
    limit = Version(version_under_test)
    answered_any = False
    for source, candidates in _examined_sources(sources, limit, version_under_test):
        for record in candidates:
            try:
                published = source.has_published_binary(record.version)
            except Exception as exc:
                print(
                    f"[{source.name}] {record.version} "
                    f"eligible={record.eligibility.value} probe-failed: {exc}"
                )
                continue
            answered_any = True
            print(
                f"[{source.name}] {record.version} "
                f"eligible={record.eligibility.value} published={published}"
            )
    return EXIT_RESOLVED if answered_any else EXIT_NOTHING_RESOLVABLE


def main(argv: Optional[Sequence[str]] = None) -> int:
    """CLI entry point; see the module docstring for the contract."""
    args = _parse_args(argv)
    _configure_logging(debug=args.debug)

    override = args.override
    if override:
        # The emergency override must work even when the discovery inputs (task
        # expansions, version file) are broken or unavailable, so it is checked
        # before any of them.
        LOGGER.info("Override set; bypassing discovery", override=override)
        print(override)
        return EXIT_RESOLVED

    try:
        config = ResolverConfig.from_env(os.environ)
    except ConfigError as exc:
        LOGGER.error(str(exc))
        return EXIT_USAGE_ERROR

    try:
        version_under_test = args.version_under_test or read_version_under_test()
        # Validate early so a malformed version is a usage error rather than a
        # crash inside resolve().
        Version(version_under_test)
    except ConfigError as exc:
        LOGGER.error(str(exc))
        return EXIT_USAGE_ERROR
    except InvalidVersion as exc:
        LOGGER.error(
            "Version under test is not a valid version",
            version_under_test=args.version_under_test,
            error=str(exc),
        )
        return EXIT_USAGE_ERROR

    sources = _build_sources(args.source, config, evg_projects=args.evg_project)

    if args.print_candidates:
        return _print_candidates(sources, version_under_test)

    if args.source != "auto" and not any(source.implemented for source in sources):
        LOGGER.error("Requested discovery source is not implemented yet", source=args.source)
        return EXIT_USAGE_ERROR

    try:
        resolved = resolve(sources, version_under_test)
    except ConfigError as exc:
        # E.g. a platform/architecture with no known atlas build variant: a
        # configuration error, not "nothing resolvable".
        LOGGER.error(str(exc))
        return EXIT_USAGE_ERROR
    if resolved is None:
        # Nothing resolvable: stdout stays empty and the caller decides fail
        # vs. skip (Inc 4 extends this with an explicit skip signal).
        LOGGER.error("No DSC release qualifies", version_under_test=version_under_test)
        return EXIT_NOTHING_RESOLVABLE

    print(resolved)
    return EXIT_RESOLVED


if __name__ == "__main__":
    sys.exit(main())
