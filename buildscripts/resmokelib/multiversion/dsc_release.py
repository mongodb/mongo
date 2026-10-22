"""Resolve the old-side DSC release for the disagg multiversion suites.

The disagg multiversion suites test the build under development against the
DSC release that production is running -- the **last published DSC train
tag**. The resolver computes it:

> the newest DSC train tag that is not at or descending from the commit under
> test, and whose DSC binary is published in the configured feed

"Older than the code under test" means **git topology**, not version-string
ordering (``9.1.0-rc1022 < 9.1.0`` is a packaging artifact, not release
ordering). DSC release trains are branches ``dsc-release-N`` in 10gen/mongo
whose builds are tagged with a continuing rc counter (``dsc-release-5`` ->
``r9.0.0-rc1013`` .. ``r9.0.0-rc1020``, ``dsc-release-6`` -> ``r9.1.0-rc1021``).
Candidates are those train tags, enumerated newest first by the shared
previous_release_tag machinery (git versionsort) and filtered to exclude tags
at or descending from HEAD -- in the task checkout HEAD is the commit being
built, so the build under test and anything built from its future can never be
the old side, however its version string compares. The probe walk is anchored
at the **base**: the train tag closest in history to the commit under test
(``find_previous_release_tag`` with the DSC glob -- git merge-base fork
distance, ties by symmetric distance and version), so a superseded train that
kept tagging cannot be probed ahead of the topologically-closest one. A tag
existing is not sufficient -- a DSC binary is not necessarily published for
every tag -- so every candidate is probed against the atlas release feed for
the requested platform/architecture/edition, stepping down (across train
boundaries) until one qualifies. The feed is the publication probe for those
candidates; candidate order comes from git topology alone.

The MONGO_VERSION comparison is a sanity assertion on the result, not a
selection rule: the resolved release must not be newer than the version the
build targets -- release-level "newer": the release under test's own
pre-releases (the DSC trains among them) qualify, its plain GA and anything
from a later release do not -- and a violation fails with
:class:`SanityError` (exit 1, stdout empty).

Contract:

* stdout carries exactly one line: the resolved version string. The caller
  (evergreen/multiversion_selection.sh) appends ``=last-patch`` and hands
  the result to db-contrib-tool. Every diagnostic goes to stderr.
* Exit codes: 0 = resolved; 1 = nothing resolvable or a failed sanity check
  (stdout stays empty; the caller decides fail vs. skip); 2 = usage/config
  error.
* All network I/O is behind :class:`VersionSource` implementations -- the same
  injected-client pattern as ``MultiversionService.last_patch_resolver`` --
  which is what makes the policy unit-testable with fake sources, no network.
  The implementations live in
  :mod:`buildscripts.resmokelib.multiversion.dsc_release_sources`: ``tags``
  enumerates the candidates from git, ``feed`` answers publication.
* An explicit override (``--override``) bypasses discovery entirely and is
  echoed verbatim. The ``multiversion_dsc_release`` expansion is the *shell's*
  emergency pin -- the resolver never reads it, so the shell can compare
  discovery against the pin.

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

    The shell owns the version pin (``multiversion_dsc_release``): discovery
    always runs, so the shell can compare its result against the pin.
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
    """Whether a candidate may be resolved.

    UNPARSEABLE candidates are reported (and shown by --print-candidates);
    only ELIGIBLE ones are resolved. The "older than the code under test"
    bound is git topology -- the sources' at/descending-HEAD exclusion; the
    version comparison is a sanity assertion on the resolved result. The value
    is the form --print-candidates renders, so the printed vocabulary has one
    definition.
    """

    ELIGIBLE = "True"
    UNPARSEABLE = "unparseable"


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


def _examine_candidates(source: VersionSource, candidates: Sequence[str]) -> Iterator[_Examined]:
    """Examine each candidate for parseability; unparseable ones are reported as such."""
    for version in candidates:
        try:
            Version(version)
        except InvalidVersion:
            LOGGER.warning(
                "Ignoring candidate with unparseable version",
                source=source.name,
                version=version,
            )
            eligibility = _Eligibility.UNPARSEABLE
        else:
            eligibility = _Eligibility.ELIGIBLE
        yield _Examined(source=source, version=version, eligibility=eligibility)


def _examined_sources(
    sources: Sequence[VersionSource],
) -> Iterator[tuple[VersionSource, Iterator[_Examined]]]:
    """Walk the discovery chain source by source, examining every candidate.

    Sources that fail to enumerate (no git repository, ...) are logged here
    (once) and skipped: they yield nothing. Lazily: --print-candidates
    consumes everything, resolve() short-circuits.
    """
    for source in sources:
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
        yield source, _examine_candidates(source, candidates)


def _published(source: VersionSource, version: str) -> bool:
    """Ask the source whether ``version`` has a published DSC binary.

    Probe failures (network errors, unavailable services) count as
    "unpublished": selection steps down. ConfigError propagates -- it is a
    usage error (exit 2).
    """
    try:
        return source.has_published_binary(version)
    except ConfigError:
        raise
    except Exception as exc:
        LOGGER.warning(
            "Availability probe failed; stepping down",
            source=source.name,
            version=version,
            error=str(exc),
        )
        return False


class SanityError(Exception):
    """The resolved release violates a sanity invariant and cannot be trusted."""


def _assert_sanity(resolved: str, version_under_test: str) -> None:
    """Assert the resolved release is not newer than the version under test.

    "Older" is release-level: the release's own pre-releases -- the alpha,
    beta and rc trains (the DSC trains among them) -- are genuinely older than
    the code under test however their channels compare ("rc" sorting above
    "alpha" is a packaging artifact, not release ordering), so a same-release
    pre-release resolves. What cannot be trusted is a candidate from a newer
    release, or the plain GA of the release under test itself (the build
    leaking through the topology bound).
    """
    resolved_version = Version(resolved)
    under_test_version = Version(version_under_test)
    if resolved_version.release > under_test_version.release or (
        resolved_version.release == under_test_version.release
        and not resolved_version.is_prerelease
    ):
        raise SanityError(
            f"sanity check failed: resolved DSC release {resolved} is not older"
            f" than the version under test {version_under_test}"
        )


def resolve(sources: Sequence[VersionSource], version_under_test: str) -> Optional[str]:
    """Return the newest published DSC train tag not at/descending from the commit under test.

    The tag-topology bound is the authoritative "older than the code under
    test": candidates arrive anchored at the base -- the train tag closest in
    history to HEAD, with tags at or descending from HEAD already excluded by
    the source -- git topology, not version-string ordering
    (``9.1.0-rc1022 < 9.1.0`` is a packaging artifact). This is the testable
    policy core:

    * candidates are consumed in the order the source returns them -- anchored
      at the base and stepping down (versionsort order from there); ordering
      is the source's responsibility;
    * a source that fails to enumerate (no git repository, ...) is skipped:
      discovery moves to the next source;
    * an availability probe failure steps down to the next candidate;
    * the first published candidate wins -- normally the base itself; the
      probe covers "tagged but not yet published" and steps down.

    The version-under-test comparison is a sanity assertion on the result: a
    resolved release that is not older than the version under test (release
    level: a same-release pre-release is older, the plain GA and any later
    release are not) means the topology bound and the version file disagree,
    and fails with :class:`SanityError`.

    The traversal is shared with --print-candidates, which reports every
    candidate; resolve() stops at the first published one.
    """
    resolved = None
    for source, candidates in _examined_sources(sources):
        for record in candidates:
            if record.eligibility is not _Eligibility.ELIGIBLE:
                continue
            if _published(source, record.version):
                LOGGER.info("Resolved DSC release", source=source.name, version=record.version)
                resolved = record.version
                break
            LOGGER.info(
                "Candidate has no published binary; stepping down",
                source=source.name,
                version=record.version,
            )
        if resolved is not None:
            break
        LOGGER.info("Source exhausted without a qualifying candidate", source=source.name)
    if resolved is None:
        return None
    _assert_sanity(resolved, version_under_test)
    return resolved


def _build_sources(config: ResolverConfig) -> list[VersionSource]:
    """Build the discovery chain: the tag-topology route, probed against the feed."""
    return [TagSource(probe=FeedSource(config.query))]


def _parse_args(argv: Optional[Sequence[str]]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Resolve the newest published DSC release for multiversion testing."
        " Prints exactly one line -- the version -- on stdout.",
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


def _print_candidates(sources: Sequence[VersionSource]) -> int:
    """List each source's candidates with publication status (incident triage).

    Consumes the same traversal as resolve(), reporting every candidate
    (including unparseable ones) and rendering probe failures. The exit code
    is EXIT_RESOLVED when at least one probe answered; an invocation that
    learned nothing -- nothing enumerated, or every probe failed -- exits
    EXIT_NOTHING_RESOLVABLE.
    """
    answered_any = False
    for source, candidates in _examined_sources(sources):
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
        # Validate early: a malformed version is a usage error (exit 2).
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

    sources = _build_sources(config)

    if args.print_candidates:
        return _print_candidates(sources)

    try:
        resolved = resolve(sources, version_under_test)
    except ConfigError as exc:
        # A configuration error is a usage error (exit 2), distinct from the
        # "nothing resolvable" signal (exit 1).
        LOGGER.error(str(exc))
        return EXIT_USAGE_ERROR
    except SanityError as exc:
        # The topology bound and MONGO_VERSION disagree: refuse the result
        # (exit 1, stdout empty).
        LOGGER.error(str(exc))
        return EXIT_NOTHING_RESOLVABLE
    if resolved is None:
        # Nothing resolvable: stdout stays empty and the caller decides fail
        # vs. skip.
        LOGGER.error("No DSC release qualifies", version_under_test=version_under_test)
        return EXIT_NOTHING_RESOLVABLE

    print(resolved)
    return EXIT_RESOLVED


if __name__ == "__main__":
    sys.exit(main())
