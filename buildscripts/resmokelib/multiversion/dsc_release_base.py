"""The interface of the DSC release resolver's discovery routes.

This module defines the contract the discovery routes implement:
:class:`VersionSource`, the :class:`DscBinaryQuery` they are parameterized
with, and :class:`ConfigError`. It depends on nothing repo-internal; the
implementations live in
:mod:`buildscripts.resmokelib.multiversion.dsc_release_sources` and the
resolution policy and CLI in
:mod:`buildscripts.resmokelib.multiversion.dsc_release` -- both depend on this
module, never the reverse.
"""

from __future__ import annotations

from abc import ABC, abstractmethod
from dataclasses import dataclass


class ConfigError(Exception):
    """Required configuration is missing or malformed (exit code 2)."""


@dataclass(frozen=True)
class DscBinaryQuery:
    """The platform/architecture/edition a published DSC binary must exist for."""

    platform: str
    architecture: str
    edition: str


class VersionSource(ABC):
    """One discovery route for DSC releases.

    All network (and git) I/O lives behind the two methods, so the resolution
    policy can be tested with fake sources and no network. A source sets
    ``implemented = False`` while its route is not wired up yet;
    :func:`resolve <buildscripts.resmokelib.multiversion.dsc_release.resolve>`
    skips those loudly, and the CLI turns an explicitly requested unimplemented
    route into a usage error.
    """

    name = "source"
    implemented = True

    @abstractmethod
    def candidate_versions(self) -> list[str]:
        """Return the DSC release versions this source knows about.

        The result may include versions whose binaries are not published;
        :func:`resolve <buildscripts.resmokelib.multiversion.dsc_release.resolve>`
        verifies publication per candidate. Return them newest first --
        :func:`resolve <buildscripts.resmokelib.multiversion.dsc_release.resolve>`
        consumes them in order and does not re-sort.
        """

    @abstractmethod
    def has_published_binary(self, version: str) -> bool:
        """Return whether ``version`` has a published DSC binary for the configured platform."""
