# Copyright (c) MongoDB, Inc.
# SPDX-License-Identifier: SSPL-1.0
"""Test cases for buildscripts/idl/generate_ifr_registry.py."""

import os
import sys
import tempfile
import textwrap
import unittest

# Allow running via pytest from the repo root.
sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import generate_ifr_registry  # noqa: E402


class TestGenerateIFRRegistry(unittest.TestCase):
    """Test generate_ifr_registry against synthetic and real IDL trees."""

    def _write(self, path, content):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write(textwrap.dedent(content))

    def test_collect_flags_picks_up_rollout_and_release(self):
        with tempfile.TemporaryDirectory() as root:
            self._write(
                os.path.join(root, "a.idl"),
                """
                feature_flags:
                    featureFlagAlpha:
                        incremental_rollout_phase: rollout
                    featureFlagBeta:
                        incremental_rollout_phase: in_development
                """,
            )
            self._write(
                os.path.join(root, "sub", "b.idl"),
                """
                feature_flags:
                    featureFlagGamma:
                        incremental_rollout_phase: release
                """,
            )
            rollout, release = generate_ifr_registry.collect_flags(root)
        self.assertEqual(rollout, {"featureFlagAlpha"})
        self.assertEqual(release, {"featureFlagGamma"})

    def test_collect_flags_raises_on_overlap(self):
        # TODO SERVER-126893 delete this test.
        with tempfile.TemporaryDirectory() as root:
            self._write(
                os.path.join(root, "one.idl"),
                """
                feature_flags:
                    featureFlagDup:
                        incremental_rollout_phase: rollout
                """,
            )
            self._write(
                os.path.join(root, "two.idl"),
                """
                feature_flags:
                    featureFlagDup:
                        incremental_rollout_phase: release
                """,
            )
            with self.assertRaisesRegex(ValueError, "featureFlagDup"):
                generate_ifr_registry.collect_flags(root)

    def test_collect_flags_ignores_non_idl_yaml_and_missing_sections(self):
        with tempfile.TemporaryDirectory() as root:
            # An IDL file without a feature_flags section.
            self._write(
                os.path.join(root, "noflags.idl"),
                """
                global:
                    cpp_namespace: mongo
                """,
            )
            # A sibling YAML file that isn't an .idl — should be skipped by the glob.
            self._write(
                os.path.join(root, "other.yml"),
                """
                feature_flags:
                    featureFlagShouldNotAppear:
                        incremental_rollout_phase: rollout
                """,
            )
            rollout, release = generate_ifr_registry.collect_flags(root)
        self.assertEqual(rollout, set())
        self.assertEqual(release, set())

    def test_render_produces_stable_sorted_output(self):
        body = generate_ifr_registry.render_registry(
            {"featureFlagB", "featureFlagA"}, {"featureFlagC"}
        )
        # Flags appear in sorted order, regardless of input set ordering.
        rollout_idx_a = body.index("featureFlagA")
        rollout_idx_b = body.index("featureFlagB")
        self.assertLess(rollout_idx_a, rollout_idx_b)
        self.assertIn("release:\n  featureFlagC: {}", body)

    def test_render_empty_sections_use_flow_style_empty_map(self):
        body = generate_ifr_registry.render_registry(set(), set())
        self.assertIn("rollout: {}", body)
        self.assertIn("release: {}", body)

    def test_collect_flags_ignores_unrelated_idl_content(self):
        """Adding non-flag content to an IDL file does not change the registry output."""
        with tempfile.TemporaryDirectory() as root:
            self._write(
                os.path.join(root, "a.idl"),
                """
                feature_flags:
                    featureFlagAlpha:
                        incremental_rollout_phase: rollout
                    featureFlagBeta:
                        incremental_rollout_phase: release
                """,
            )
            rollout_before, release_before = generate_ifr_registry.collect_flags(root)

            # Add unrelated content before, after, and between the flags.
            self._write(
                os.path.join(root, "a.idl"),
                """
                global:
                    cpp_namespace: mongo
                server_parameters:
                    someParam:
                        set_at: startup
                feature_flags:
                    featureFlagAlpha:
                        incremental_rollout_phase: rollout
                    featureFlagUnrelated:
                        incremental_rollout_phase: in_development
                    featureFlagBeta:
                        incremental_rollout_phase: release
                commands:
                    someCommand:
                        description: "a command"
                """,
            )
            rollout_after, release_after = generate_ifr_registry.collect_flags(root)

        self.assertEqual(rollout_before, rollout_after)
        self.assertEqual(release_before, release_after)

    def test_collect_flags_picks_up_new_flag_added_to_existing_idl(self):
        """Adding a new rollout flag to an IDL that already has flags registers it."""
        with tempfile.TemporaryDirectory() as root:
            self._write(
                os.path.join(root, "a.idl"),
                """
                feature_flags:
                    featureFlagAlpha:
                        incremental_rollout_phase: rollout
                """,
            )
            rollout_before, _ = generate_ifr_registry.collect_flags(root)
            self.assertEqual(rollout_before, {"featureFlagAlpha"})

            # Add a second rollout flag to the same file.
            self._write(
                os.path.join(root, "a.idl"),
                """
                feature_flags:
                    featureFlagAlpha:
                        incremental_rollout_phase: rollout
                    featureFlagBeta:
                        incremental_rollout_phase: rollout
                """,
            )
            rollout_after, _ = generate_ifr_registry.collect_flags(root)

        self.assertEqual(rollout_after, {"featureFlagAlpha", "featureFlagBeta"})


class TestRegistryStaleness(unittest.TestCase):
    """Ensure the committed ifr_flag_registry.yml matches what the IDL sources produce."""

    # Resolve repo root: this file lives at buildscripts/idl/tests/
    _REPO_ROOT = os.path.normpath(
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..")
    )

    def test_committed_registry_is_up_to_date(self):
        source_root = os.path.join(self._REPO_ROOT, generate_ifr_registry.DEFAULT_SOURCE_ROOT)
        registry_path = os.path.join(self._REPO_ROOT, generate_ifr_registry.DEFAULT_REGISTRY_PATH)

        rollout, release = generate_ifr_registry.collect_flags(source_root)
        expected = generate_ifr_registry.render_registry(rollout, release)

        self.assertTrue(
            os.path.exists(registry_path),
            f"{generate_ifr_registry.DEFAULT_REGISTRY_PATH} does not exist. "
            f"Run: python3 buildscripts/idl/generate_ifr_registry.py",
        )

        with open(registry_path, encoding="utf-8") as f:
            actual = f.read()

        if actual != expected:
            # Build a human-friendly summary of what changed.
            actual_lines = set(actual.splitlines())
            expected_lines = set(expected.splitlines())
            missing = sorted(expected_lines - actual_lines)
            extra = sorted(actual_lines - expected_lines)
            details = []
            if missing:
                details.append("Lines missing from committed file:\n  " + "\n  ".join(missing))
            if extra:
                details.append("Extra lines in committed file:\n  " + "\n  ".join(extra))
            diff_summary = "\n".join(details) if details else "(content differs)"

            self.fail(
                f"{generate_ifr_registry.DEFAULT_REGISTRY_PATH} is out of date.\n"
                f"\n"
                f"{diff_summary}\n"
                f"\n"
                f"To fix, run:\n"
                f"  python3 buildscripts/idl/generate_ifr_registry.py\n"
            )


if __name__ == "__main__":
    unittest.main()
