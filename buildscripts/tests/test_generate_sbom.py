#!/usr/bin/env python3
"""
Tests for buildscripts/sbom/*.py
"""

import logging
import os
import sys
import unittest

from buildscripts.sbom.config import get_semver_from_release_version, regex_semver
from buildscripts.sbom.endorctl_utils import EndorCtl
from buildscripts.sbom.sbom_utils import (
    SOURCE_METADATA,
    add_team_responsible,
    is_valid_purl,
    propagate_team_ownership,
    reconcile_dependency_refs,
    tag_unsourced_team_owner,
)
from buildscripts.tests.sbom_linter.sbom_linter import load_metadata

logging.basicConfig(level=logging.INFO, stream=sys.stdout)


class TestEndorctl(unittest.TestCase):
    """Test cases for the EndorCtl class."""

    def test_endorctl_init(self):
        """Tests the Endorctl constructor."""
        e = EndorCtl(namespace="mongodb.10gen", retry_limit=1, sleep_duration=5)
        self.assertEqual(e.namespace, "mongodb.10gen")
        self.assertEqual(e.retry_limit, 1)
        self.assertEqual(e.sleep_duration, 5)

    def test_call_endorctl_missing(self):
        """Tests EndorCtl execution with endorctl not in path."""
        logger = logging.getLogger("generate_sbom")
        logger.setLevel(logging.INFO)

        e = EndorCtl(namespace="mongodb.10gen", endorctl_path="this_path_does_not_exist")
        result = e.get_sbom("https://github.com/10gen/mongo.git")
        self.assertRaises(FileNotFoundError)
        self.assertIsNone(result, None)


class TestConfigRegex(unittest.TestCase):
    """Test suite for configuration regex patterns and PURL validation.

    This test class validates regex patterns used for semantic versioning,
    version extraction from release strings, and Package URL (PURL) validation.
    """

    def test_semver_regex(self):
        """Tests the regex_semver."""

        # List of valid semantic version strings
        valid_semvers = [
            "0.0.1",
            "1.2.3",
            "10.20.30",
            "1.2.3-alpha",
            "1.2.3-alpha.1",
            "1.2.3-0.beta",
            "1.2.3+build.123",
            "1.2.3-rc.1+build.456",
            "1.0.0-beta+exp.sha.5114f85",
        ]

        # List of invalid semantic version strings
        invalid_semvers = [
            "1.2",  # Incomplete
            "1",  # Incomplete
            "v1.2.3",  # Has a 'v' prefix (regex is for the version part only)
            "1.2.3-",  # Trailing hyphen in pre-release
            "1.2.3+",  # Trailing plus in build
            "1.02.3",  # Leading zero in minor component
            "1.2.03",  # Leading zero in patch component
            "alpha",  # Not a valid version
            "1.2.3.4",  # Four components (SemVer is 3)
            "1.2.3-alpha_beta",  # Underscore in pre-release
        ]

        print("\nTesting regex_semver:")
        for v in valid_semvers:
            with self.subTest(v=v):
                self.assertIsNotNone(
                    regex_semver.fullmatch(v), f"Expected '{v}' to be a valid semver"
                )

        for v in invalid_semvers:
            with self.subTest(v=v):
                self.assertIsNone(
                    regex_semver.fullmatch(v), f"Expected '{v}' to be an invalid semver"
                )

    def test_get_semver_from_release_version(self):
        """Tests the transformation function that uses VERSION_PATTERN_REPL."""

        # (input, expected_output)
        test_cases = [
            # Pattern 1: 'debian/1.28.1-1'
            ("debian/1.28.1-1", "1.28.1"),
            ("debian/1.2.3-rc.1-2", "1.2.3-rc.1"),
            # Pattern 2: 'gperftools-2.9.1', 'mongo/v1.5.2', etc.
            ("gperftools-2.9.1", "2.9.1"),
            ("mongo/v1.5.2", "1.5.2"),
            ("mongodb-8.2.0-alpha2", "8.2.0-alpha2"),
            ("release-1.12.0", "1.12.0"),
            ("yaml-cpp-0.6.3", "0.6.3"),
            ("mongo/1.2.3-beta+build", "1.2.3-beta+build"),
            # Pattern 3: 'asio-1-34-2', 'cares-1_27_0'
            ("asio-1-34-2", "1.34.2"),
            ("cares-1_27_0", "1.27.0"),
            # Pattern 4: 'pcre2-10.40'
            ("pcre2-10.40", "10.40"),
            ("something-1.2", "1.2"),
            # Pattern 5: 'icu-release-57-1'
            ("icu-release-57-1", "57.1"),
            ("foo-bar-12-3", "12.3"),
            # Pattern 6: 'v2.6.0'
            ("v2.6.0", "2.6.0"),
            ("v1.2.3-alpha.1", "1.2.3-alpha.1"),
            # Pattern 7: 'r2.5.1'
            ("r2.5.1", "2.5.1"),
            ("r1.2.3-alpha.1", "1.2.3-alpha.1"),
            # Pattern 7: 'v2025.04.21.00' (non-semver but specific pattern)
            ("v2025.04.21.00", "2025.04.21.00"),
            # --- Cases that should not match ---
            ("1.2.3", "1.2.3"),  # Already clean
            ("latest", "latest"),  # No match
            ("not-a-version", "not-a-version"),  # No match
            ("v1.2", "v1.2"),  # Not matched by any pattern
        ]

        print("\nTesting get_semver_from_release_version():")
        for input_str, expected_str in test_cases:
            with self.subTest(input=input_str):
                result = get_semver_from_release_version(input_str)
                self.assertEqual(
                    result,
                    expected_str,
                    f"Input: '{input_str}', Expected: '{expected_str}', Got: '{result}'",
                )

    def test_purls_valid(self):
        """Tests valid PURLs."""
        valid_purls = [
            "pkg:github/gperftools/gperftools@gperftools-2.9.1",
            "pkg:github/mongodb/mongo-c-driver@1.23.4",
            "pkg:github/google/benchmark",  # No version
            "pkg:github/c-ares/c-ares@cares-1_27_0",
            "pkg:github/apache/avro@release-1.12.0",
            "pkg:github/jbeder/yaml-cpp@yaml-cpp-0.6.3",
            "pkg:github/pcre2project/pcre2@pcre2-10.40",
            "pkg:github/unicode-org/icu@icu-release-57-1",
            "pkg:github/confluentinc/librdkafka@v2.6.0",
            "pkg:github/facebook/folly@v2025.04.21.00?foo=bar#src/main",  # With qualifiers/subpath
            "pkg:generic/valgrind/valgrind@3.23.0",  # namespace/name@version
            "pkg:generic/intel/IntelRDFPMathLib@2.0u2",
            "pkg:generic/openldap/openldap",  # namespace/name
            "pkg:generic/openssl@3.0.13",  # name@version
            "pkg:generic/my-package",  # name only
            "pkg:generic/my-package@1.2.3?arch=x86_64#README.md",  # With qualifiers/subpath
            "pkg:deb/debian/firefox-esr@128.11.0esr-1?arch=source",
            "pkg:pypi/ocspbuilder@0.10.2",
        ]

        print("\nTesting Valid PURLs:")
        for purl in valid_purls:
            with self.subTest(purl=purl):
                self.assertTrue(is_valid_purl(purl), f"Expected '{purl}' to be valid")

    def test_purls_invalid(self):
        """Tests invalid PURLs."""
        invalid_purls = [
            "pkg:github/gperftools",  # Missing name
            "pkg:github/",  # Missing namespace and name
            "pkg:c/github.com/abseil/abseil-cpp",  # Wrong type (from your config.py)
            "pkg:github/mongodb/mongo-c-driver@1.2.3@4.5.6",  # Double version
            "pkg:generic/github/mongodb/mongo",  # Wrong type
            "pkg:generic/",  # Missing name
            "pkg:github/valgrind/",  # Missing name
            "pkg:generic/my-package@1.2@3.4",  # Double version
            "pkg:generic/spaces in name",  # Spaces not allowed (must be encoded)
            "pkg:deb/firefox-esr@128.11.0esr-1?arch=source",  # Missing vendor
            "pkg:pypi/ocsp/ocspbuilder@0.10.2",  # no namespace for PyPI
        ]

        print("\nTesting Invalid PURLs:")
        for purl in invalid_purls:
            with self.subTest(purl=purl):
                self.assertFalse(is_valid_purl(purl), f"Expected '{purl}' to be invalid")


class TestMetadataFile(unittest.TestCase):
    """Unit tests for SBOM metadata file validation and version tag consistency."""

    METADATA_FILE = os.path.join("buildscripts", "sbom", "metadata.cdx.yaml")
    VERSION_TAG = "{{VERSION}}"

    def test_metadata_sbom_version_tags(self):
        """Test that SBOM metadata components have consistent version tags.

        Verifies that each component in the metadata SBOM file contains required fields
        (bom-ref and version) plus at least one of purl or cpe. Additionally ensures that
        the VERSION_TAG is either present in all component properties or absent from all,
        maintaining consistency across bom-ref, version, purl, and cpe fields.
        """
        meta_bom = load_metadata(self.METADATA_FILE)
        for component in meta_bom["components"]:
            with self.subTest(component=component):
                properties = []
                properties.append(component["bom-ref"])
                properties.append(component["version"])
                if "purl" in component:
                    properties.append(component["purl"])
                if "cpe" in component:
                    properties.append(component["cpe"])
                # make sure component has a minimum of bom-ref, version and at least one of purl or cpe
                self.assertGreater(
                    len(properties),
                    2,
                    f"Component must have a minimum of bom-ref, version and at least one of purl or cpe. {properties}",
                )
                # make sure all properites either have version tag or no version tags
                self.assertTrue(
                    all(self.VERSION_TAG in p for p in properties)
                    or all(self.VERSION_TAG not in p for p in properties),
                    f"Component must have version tag '{self.VERSION_TAG}' in all or none of bom-ref, version and purl and/or cpe. {properties})",
                )


class TestReconcileDependencyRefs(unittest.TestCase):
    """Tests for sbom_utils.reconcile_dependency_refs."""

    def _make_sbom(self, components, meta_component, dependencies):
        return {
            "metadata": {"component": meta_component},
            "components": components,
            "dependencies": dependencies,
        }

    def test_adds_stub_for_component_missing_from_dependencies(self):
        sbom = self._make_sbom(
            components=[{"bom-ref": "pkg:npm/foo@1.0"}],
            meta_component={"bom-ref": "pkg:github/org/repo@main"},
            dependencies=[{"ref": "pkg:github/org/repo@main", "dependsOn": []}],
        )
        reconcile_dependency_refs(sbom)
        refs = {d["ref"] for d in sbom["dependencies"]}
        self.assertIn("pkg:npm/foo@1.0", refs)

    def test_removes_orphaned_top_level_ref(self):
        sbom = self._make_sbom(
            components=[],
            meta_component={"bom-ref": "pkg:github/org/repo@main"},
            dependencies=[
                {"ref": "pkg:github/org/repo@main", "dependsOn": []},
                {"ref": "pkg:cargo/gone@1.0", "dependsOn": []},
            ],
        )
        reconcile_dependency_refs(sbom)
        refs = {d["ref"] for d in sbom["dependencies"]}
        self.assertNotIn("pkg:cargo/gone@1.0", refs)

    def test_orphaned_depends_on_entries_are_not_pruned_by_reconcile(self):
        """reconcile_dependency_refs removes orphaned top-level refs but not dependsOn entries;
        that cleanup is done separately in generate_sbom.py before writing the private SBOM."""
        sbom = self._make_sbom(
            components=[{"bom-ref": "pkg:npm/foo@1.0"}],
            meta_component={"bom-ref": "pkg:github/org/repo@main"},
            dependencies=[
                {
                    "ref": "pkg:github/org/repo@main",
                    "dependsOn": ["pkg:npm/foo@1.0", "pkg:cargo/gone@1.0"],
                },
                {"ref": "pkg:npm/foo@1.0", "dependsOn": []},
            ],
        )
        reconcile_dependency_refs(sbom)
        root_dep = next(d for d in sbom["dependencies"] if d["ref"] == "pkg:github/org/repo@main")
        # The orphaned dependsOn ref survives reconcile — the caller must prune it.
        self.assertIn("pkg:cargo/gone@1.0", root_dep["dependsOn"])

    def test_generate_sbom_depends_on_pruning_logic(self):
        """Simulate the inline pruning in generate_sbom.py that runs before write."""
        meta_bom = {
            "metadata": {"component": {"bom-ref": "pkg:github/org/repo@main"}},
            "components": [{"bom-ref": "pkg:npm/foo@1.0"}],
            "dependencies": [
                {
                    "ref": "pkg:github/org/repo@main",
                    "dependsOn": ["pkg:npm/foo@1.0", "pkg:cargo/gone@1.0"],
                },
                {"ref": "pkg:npm/foo@1.0", "dependsOn": ["pkg:cargo/also-gone@2.0"]},
            ],
        }

        final_component_refs = {meta_bom["metadata"]["component"]["bom-ref"]} | {
            c["bom-ref"] for c in meta_bom["components"]
        }
        pruned_count = 0
        for dep in meta_bom.get("dependencies", []):
            before = len(dep.get("dependsOn", []))
            dep["dependsOn"] = [r for r in dep.get("dependsOn", []) if r in final_component_refs]
            pruned_count += before - len(dep["dependsOn"])

        self.assertEqual(pruned_count, 2)
        root = next(d for d in meta_bom["dependencies"] if d["ref"] == "pkg:github/org/repo@main")
        self.assertEqual(root["dependsOn"], ["pkg:npm/foo@1.0"])
        foo = next(d for d in meta_bom["dependencies"] if d["ref"] == "pkg:npm/foo@1.0")
        self.assertEqual(foo["dependsOn"], [])


class TestPropagateTeamOwnership(unittest.TestCase):
    """propagate_team_ownership() inherits owners only where unambiguous."""

    TEAM = "internal:team_responsible"
    SOURCE = "internal:team_responsible_source"
    CONSUMERS = "internal:team_consumers"

    @staticmethod
    def _bom(owners: dict[str, str | list[str]], edges: dict[str, list[str]]) -> dict:
        refs = set(owners) | set(edges) | {r for deps in edges.values() for r in deps}
        components = []
        for ref in sorted(refs):
            component = {"bom-ref": ref, "name": ref}
            teams = owners.get(ref, [])
            for team in [teams] if isinstance(teams, str) else teams:
                component.setdefault("properties", []).append(
                    {"name": "internal:team_responsible", "value": team}
                )
            components.append(component)
        dependencies = [{"ref": r, "dependsOn": edges.get(r, [])} for r in sorted(refs)]
        return {"components": components, "dependencies": dependencies}

    def _teams(self, bom: dict, ref: str) -> list[str]:
        component = next(c for c in bom["components"] if c["bom-ref"] == ref)
        return [p["value"] for p in component.get("properties", []) if p["name"] == self.TEAM]

    def _values(self, bom: dict, ref: str, name: str) -> list[str]:
        component = next(c for c in bom["components"] if c["bom-ref"] == ref)
        return [p["value"] for p in component.get("properties", []) if p["name"] == name]

    def _sources(self, bom: dict, ref: str) -> list[str]:
        return self._values(bom, ref, self.SOURCE)

    def test_linear_chain_inherits(self):
        bom = self._bom({"owned": "A"}, {"owned": ["a"], "a": ["b"]})
        assigned = propagate_team_ownership(bom)
        self.assertEqual(assigned, {"a": "A", "b": "A"})
        self.assertEqual(self._teams(bom, "b"), ["A"])
        self.assertEqual(self._sources(bom, "b"), ["inherited:owned"])
        self.assertEqual(self._sources(bom, "owned"), [])

    def test_inherited_source_is_the_versionless_purl_of_the_owned_ancestor(self):
        bom = self._bom({"pkg:maven/g/owner@1.0": "A"}, {"pkg:maven/g/owner@1.0": ["pkg:x/y@2"]})
        propagate_team_ownership(bom)
        self.assertEqual(self._sources(bom, "pkg:x/y@2"), ["inherited:pkg:maven/g/owner"])

    def test_source_is_deterministic_when_owners_share_a_team(self):
        bom = self._bom({"o2": "A", "o1": "A"}, {"o1": ["shared"], "o2": ["shared"]})
        propagate_team_ownership(bom)
        self.assertEqual(self._sources(bom, "shared"), ["inherited:o1"])

    def test_diamond_with_same_team_is_tagged(self):
        bom = self._bom(
            {"o1": "A", "o2": "A"},
            {"o1": ["shared"], "o2": ["shared"]},
        )
        self.assertEqual(propagate_team_ownership(bom), {"shared": "A"})

    def test_shared_dependency_of_two_teams_records_consumers_only(self):
        bom = self._bom({"o1": "B", "o2": "A"}, {"o1": ["shared"], "o2": ["shared"]})
        with self.assertLogs("generate_sbom", level="INFO") as logs:
            self.assertEqual(propagate_team_ownership(bom), {})
        self.assertEqual(self._teams(bom, "shared"), [])
        self.assertEqual(self._sources(bom, "shared"), [])
        self.assertEqual(self._values(bom, "shared", self.CONSUMERS), ["A|B"])
        self.assertTrue(any("multiple teams" in line for line in logs.output))

    def test_child_of_multi_consumer_node_stays_without_owner(self):
        bom = self._bom(
            {"o1": "A", "o2": "B"},
            {"o1": ["shared"], "o2": ["shared"], "shared": ["child"]},
        )
        self.assertEqual(propagate_team_ownership(bom), {})
        self.assertEqual(self._teams(bom, "child"), [])
        self.assertEqual(self._values(bom, "child", self.CONSUMERS), ["A|B"])

    def test_explicit_owner_is_kept_and_reseeds_its_children(self):
        bom = self._bom(
            {"o1": "A", "o2": "B"},
            {"o1": ["o2"], "o2": ["leaf"]},
        )
        self.assertEqual(propagate_team_ownership(bom), {"leaf": "B"})
        self.assertEqual(self._teams(bom, "o2"), ["B"])

    def test_multi_team_owner_passes_teams_to_children_as_consumers(self):
        bom = self._bom({"o1": ["B", "A"]}, {"o1": ["leaf"]})
        self.assertEqual(propagate_team_ownership(bom), {})
        self.assertEqual(self._teams(bom, "leaf"), [])
        self.assertEqual(self._values(bom, "leaf", self.CONSUMERS), ["A|B"])

    def test_cycle_terminates(self):
        bom = self._bom({"o1": "A"}, {"o1": ["a"], "a": ["b"], "b": ["a", "o1"]})
        self.assertEqual(propagate_team_ownership(bom), {"a": "A", "b": "A"})

    def test_unowned_root_is_not_a_source(self):
        bom = self._bom({}, {"root": ["a"], "a": ["b"]})
        self.assertEqual(propagate_team_ownership(bom), {})

    def test_dependency_ref_without_component_is_ignored(self):
        bom = self._bom({"o1": "A"}, {"o1": ["a"]})
        bom["dependencies"][0]["dependsOn"].append("pkg:missing@1")
        self.assertEqual(propagate_team_ownership(bom), {"a": "A"})

    def test_idempotent_and_recomputes_inherited_values(self):
        bom = self._bom({"o1": "A"}, {"o1": ["a"]})
        propagate_team_ownership(bom)
        # A new consumer from another team appears; the stale inherited owner must not stick.
        bom["components"].append(
            {"bom-ref": "o2", "properties": [{"name": self.TEAM, "value": "B"}]}
        )
        bom["dependencies"].append({"ref": "o2", "dependsOn": ["a"]})
        self.assertEqual(propagate_team_ownership(bom), {})
        self.assertEqual(self._teams(bom, "a"), [])
        # Re-running on a stable graph gives the same single owner without duplicating properties.
        bom["dependencies"][-1]["dependsOn"] = []
        self.assertEqual(propagate_team_ownership(bom), {"a": "A"})
        self.assertEqual(propagate_team_ownership(bom), {"a": "A"})
        self.assertEqual(self._teams(bom, "a"), ["A"])
        self.assertEqual(self._sources(bom, "a"), ["inherited:o1"])
        self.assertEqual(self._values(bom, "a", self.CONSUMERS), [])

    def test_explicit_owner_source_is_never_dropped_on_rerun(self):
        bom = self._bom({}, {"o1": ["a"]})
        add_team_responsible(bom["components"][1], "A", "CODEOWNERS:master")
        propagate_team_ownership(bom)
        propagate_team_ownership(bom)
        self.assertEqual(self._teams(bom, "o1"), ["A"])
        self.assertEqual(self._sources(bom, "o1"), ["CODEOWNERS:master"])
        self.assertEqual(self._sources(bom, "a"), ["inherited:o1"])


class TestTeamOwnerSource(unittest.TestCase):
    """Provenance recorded next to internal:team_responsible."""

    @staticmethod
    def _values(component: dict, name: str) -> list[str]:
        return [p["value"] for p in component.get("properties", []) if p["name"] == name]

    def test_codeowners_owner_records_its_source(self):
        component = {"bom-ref": "c"}
        add_team_responsible(component, "A", "CODEOWNERS:v8.0-staging")
        self.assertEqual(self._values(component, "internal:team_responsible"), ["A"])
        self.assertEqual(
            self._values(component, "internal:team_responsible_source"),
            ["CODEOWNERS:v8.0-staging"],
        )

    def test_same_source_is_recorded_once_for_several_teams(self):
        component = {"bom-ref": "c"}
        add_team_responsible(component, "A", "CODEOWNERS:master")
        add_team_responsible(component, "B", "CODEOWNERS:master")
        self.assertEqual(self._values(component, "internal:team_responsible"), ["A", "B"])
        self.assertEqual(
            self._values(component, "internal:team_responsible_source"), ["CODEOWNERS:master"]
        )

    def test_same_team_from_same_source_is_not_duplicated(self):
        component = {"bom-ref": "c"}
        add_team_responsible(component, "A", "CODEOWNERS:master")
        add_team_responsible(component, "A", "CODEOWNERS:master")
        self.assertEqual(self._values(component, "internal:team_responsible"), ["A"])
        self.assertEqual(
            self._values(component, "internal:team_responsible_source"), ["CODEOWNERS:master"]
        )

    def test_pre_existing_owner_is_attributed_to_metadata(self):
        component = {
            "bom-ref": "c",
            "properties": [{"name": "internal:team_responsible", "value": "M"}],
        }
        add_team_responsible(component, "A", "CODEOWNERS:master")
        self.assertEqual(
            self._values(component, "internal:team_responsible_source"),
            [SOURCE_METADATA, "CODEOWNERS:master"],
        )

    def test_tag_unsourced_owner_is_idempotent_and_ignores_unowned(self):
        owned = {
            "bom-ref": "c",
            "properties": [{"name": "internal:team_responsible", "value": "M"}],
        }
        tag_unsourced_team_owner(owned)
        tag_unsourced_team_owner(owned)
        self.assertEqual(self._values(owned, "internal:team_responsible_source"), [SOURCE_METADATA])
        unowned = {"bom-ref": "u"}
        tag_unsourced_team_owner(unowned)
        self.assertNotIn("properties", unowned)


if __name__ == "__main__":
    unittest.main(verbosity=2)
