"""Resolve and write code ownership for generated matrix suites.

Kept separate from suitesconfig.py so that OWNERS.yml parsing, pattern
matching, and file writing are independently testable without constructing
any suite-config machinery.
"""

import fnmatch
import os
from typing import Optional

import yaml

from buildscripts.resmokelib.utils import load_yaml_file

# (pattern, approvers) pair parsed from an OWNERS.yml filters list.
_Filter = tuple[str, list[str]]


class _QuotedPattern(str):
    """Marker type so filter-pattern keys render quoted, matching hand-authored OWNERS.yml files."""


class _OwnersDumper(yaml.SafeDumper):
    """Dumper that renders filter entries in the hand-authored OWNERS.yml style:

    a quoted pattern key with an omitted (blank) null value, e.g. ``- "/foo.yml":``,
    instead of PyYAML's defaults of an unquoted key and an explicit ``null`` value.
    """


def _represent_none(dumper: yaml.SafeDumper, _data: None) -> yaml.Node:
    return dumper.represent_scalar("tag:yaml.org,2002:null", "")


def _represent_quoted_pattern(dumper: yaml.SafeDumper, data: _QuotedPattern) -> yaml.Node:
    return dumper.represent_scalar("tag:yaml.org,2002:str", str(data), style='"')


_OwnersDumper.add_representer(type(None), _represent_none)
_OwnersDumper.add_representer(_QuotedPattern, _represent_quoted_pattern)


class SuiteOwnersGenerator:
    """Resolves and writes code ownership for one matrix-suite directory.

    Instantiate once per suite directory.  The constructor normalises the
    path via ``os.path.realpath`` and loads both OWNERS.yml filter files,
    so repeated calls to ``resolve_suite_owners`` are cache-free and there
    is no risk of relative/absolute path mismatches poisoning the cache key.
    """

    def __init__(self, suite_dir: str) -> None:
        # Anchor everything to a single realpath so callers can't poison the
        # key with relative paths or symlinks.
        self._suite_dir = os.path.realpath(suite_dir)
        suites_owners = os.path.join(os.path.dirname(self._suite_dir), "suites", "OWNERS.yml")
        mappings_owners = os.path.join(self._suite_dir, "mappings", "OWNERS.yml")
        self._suite_filters: list[_Filter] = self._parse_filters(suites_owners)
        self._mapping_filters: list[_Filter] = self._parse_filters(mappings_owners)

    def resolve_suite_owners(self, suite_name: str, mapping_data: Optional[dict]) -> list[str]:
        """Return the approver list for a generated suite, or [] if default-owned.

        Two tiers (first non-empty result wins):
        1. Explicit pattern in mappings/OWNERS.yml matching the suite name.
        2. Owner of the suite's ``base_suite`` in suites/OWNERS.yml.
        """
        owners = self._match(suite_name, self._mapping_filters)
        if owners:
            return owners
        base_suite = mapping_data.get("base_suite") if isinstance(mapping_data, dict) else None
        if base_suite:
            owners = self._match(base_suite, self._suite_filters)
        return owners

    def write_generated_owners_yml(self, entries: list[tuple[str, list[str]]]) -> None:
        """Write ``generated_suites/OWNERS.yml``, skipping manually-overridden suites.

        Suites matched by an entry in ``matrix_suites/OWNERS.yml`` are skipped so
        the hand-maintained entry stays authoritative.
        """
        overrides = self._manual_overrides()
        out_entries: list[tuple[str, list[str]]] = []
        skipped = 0
        for suite_name, owners in sorted(entries):
            fname = suite_name + ".yml"
            if any(fnmatch.fnmatch(fname, pat) for pat in overrides):
                skipped += 1
                continue
            # Copy so PyYAML doesn't alias identical approvers lists shared by
            # multiple suites (it dedupes by object identity, not by value).
            out_entries.append((f"/{fname}", list(owners)))

        output_path = os.path.join(self._suite_dir, "generated_suites", "OWNERS.yml")
        self._write_owners_yml(output_path, out_entries)
        print(
            f"Generated {output_path} "
            f"({len(out_entries)} auto entries, {skipped} manual overrides skipped)"
        )

    # ── private helpers ───────────────────────────────────────────────────────

    def _manual_overrides(self) -> list[str]:
        """Return generated_suites glob patterns declared in matrix_suites/OWNERS.yml.

        These entries take precedence over the auto-generated file.

        TODO(DEVPROD-35071): Hoist them into mappings/OWNERS.yml or suites/OWNERS.yml
          so all ownership is derivable, then delete this method and the skip logic.
        """
        path = os.path.join(self._suite_dir, "OWNERS.yml")
        try:
            data = load_yaml_file(path)
        except (FileNotFoundError, ValueError, OSError):
            return []
        patterns: list[str] = []
        for entry in (data or {}).get("filters", []):
            if not isinstance(entry, dict):
                continue
            for key in entry:
                if key in ("approvers", "metadata", "options", "emeritus_approvers"):
                    continue
                norm = key.lstrip("/")
                if norm.startswith("generated_suites/"):
                    patterns.append(norm[len("generated_suites/") :])
        return patterns

    @staticmethod
    def _parse_filters(path: str) -> list[_Filter]:
        """Parse ``(pattern, approvers)`` pairs from an OWNERS.yml file.

        Skips the catch-all ``*`` entry and entries whose only approver is
        ``mongo-default-approvers`` (i.e. still unowned).
        """
        try:
            data = load_yaml_file(path)
        except (FileNotFoundError, ValueError, OSError):
            return []
        result: list[_Filter] = []
        for entry in (data or {}).get("filters", []):
            if not isinstance(entry, dict):
                continue
            approvers = entry.get("approvers", [])
            real_approvers = [a for a in approvers if a != "10gen/mongo-default-approvers"]
            if not real_approvers:
                continue
            for key in entry:
                if (
                    key not in ("approvers", "metadata", "options", "emeritus_approvers")
                    and key != "*"
                ):
                    result.append((key, real_approvers))
        return result

    @staticmethod
    def _match(suite_name: str, filters: list[_Filter]) -> list[str]:
        """Return owners for the most-specific matching pattern (longest non-wildcard chars).

        Longer fixed-character spans beat shorter ones, so a narrow pattern like
        ``sharding_pqs*`` wins over a broad ``shard*``.
        """
        fname = suite_name + ".yml"
        best: list[str] = []
        best_spec = -1
        for pattern, approvers in filters:
            pat = pattern.lstrip("/")
            if fnmatch.fnmatch(fname, pat):
                spec = len(pat.replace("*", ""))
                if spec > best_spec:
                    best = approvers
                    best_spec = spec
        return best

    @staticmethod
    def _write_owners_yml(path: str, entries: list[tuple[str, list[str]]]) -> None:
        """Write an OWNERS.yml v2.0.0 file at ``path``.

        Every entry is an exact ``/<suite>.yml`` pattern (leading slash, no
        wildcards), which the codeowners parser resolves identically under
        v1.0.0 and v2.0.0, so the bump is purely "track the latest format".
        See docs/owners/owners_format.md.
        """
        header = (
            "# This file is AUTO-GENERATED by 'buildscripts/resmoke.py generate-matrix-suites'.\n"
            "# Do not edit it manually. To change ownership:\n"
            "#   - Most suites: edit buildscripts/resmokeconfig/matrix_suites/mappings/OWNERS.yml\n"
            "#     or buildscripts/resmokeconfig/suites/OWNERS.yml (for the base suite).\n"
            "#   - To override one suite: add an entry to\n"
            "#     buildscripts/resmokeconfig/matrix_suites/OWNERS.yml (it takes precedence).\n"
            "# Then re-run: buildscripts/resmoke.py generate-matrix-suites\n"
        )
        # Each filter is the OWNERS shape the codeowners parser expects: the
        # pattern as a key (its value is unused) plus a sibling ``approvers`` list.
        owners_yml = {
            "version": "2.0.0",
            "filters": [
                {_QuotedPattern(pattern): None, "approvers": approvers}
                for pattern, approvers in entries
            ],
        }
        with open(path, "w") as f:
            f.write(header)
            yaml.dump(
                owners_yml, f, Dumper=_OwnersDumper, sort_keys=False, default_flow_style=False
            )
