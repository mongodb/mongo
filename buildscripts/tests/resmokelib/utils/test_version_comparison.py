"""Tests for version_comparison.compare_bin_versions (matches MongoRunner.compareBinVersions semantics)."""

import unittest

from buildscripts.resmokelib.utils import version_comparison


class TestCompareBinVersions(unittest.TestCase):
    """Test compare_bin_versions matches JS MongoRunner.compareBinVersions behavior."""

    def test_equal_major_minor(self):
        """x.y compares equal to x.y.z (compare only up to shorter length)."""
        self.assertEqual(version_comparison.compare_bin_versions("3.2", "3.2.4"), 0)
        self.assertEqual(version_comparison.compare_bin_versions("3.2.4", "3.2"), 0)
        self.assertEqual(version_comparison.compare_bin_versions("8.3", "8.3.0"), 0)

    def test_higher_lower(self):
        """Strict comparison when components differ."""
        self.assertEqual(version_comparison.compare_bin_versions("3.2", "3.0"), 1)
        self.assertEqual(version_comparison.compare_bin_versions("3.0.9", "3.2.9"), -1)
        self.assertEqual(version_comparison.compare_bin_versions("8.3", "8.2"), 1)
        self.assertEqual(version_comparison.compare_bin_versions("8.2", "8.3"), -1)

    def test_githash_treated_as_extra_element(self):
        """Last component can be split on '-'; compare only up to min length."""
        self.assertEqual(version_comparison.compare_bin_versions("3.4", "3.4.0-abcd"), 0)
        self.assertEqual(version_comparison.compare_bin_versions("3.4.0", "3.4.0-abcd"), 0)
        self.assertEqual(version_comparison.compare_bin_versions("3.6.0", "3.4.0-abcd"), 1)
        self.assertEqual(version_comparison.compare_bin_versions("3.4.1", "3.4.0-abcd"), 1)
        self.assertEqual(version_comparison.compare_bin_versions("3.4.0-abc", "3.4.1-xyz"), -1)

    def test_differ_only_by_githash_raises(self):
        """When versions differ only by non-numeric (e.g. githash), raise."""
        with self.assertRaises(ValueError) as ctx:
            version_comparison.compare_bin_versions("3.4.1-abc", "3.4.1-xyz")
        self.assertIn("Cannot compare non-equal non-numeric", str(ctx.exception))

    def test_empty_version_raises(self):
        """Empty version string raises."""
        with self.assertRaises(ValueError):
            version_comparison.compare_bin_versions("", "8.3")
        with self.assertRaises(ValueError):
            version_comparison.compare_bin_versions("8.3", "")

    def test_single_component_raises(self):
        """At least two components required (e.g. '3' is invalid)."""
        with self.assertRaises(ValueError) as ctx:
            version_comparison.compare_bin_versions("3", "3.2")
        self.assertIn("at least two components", str(ctx.exception))
        with self.assertRaises(ValueError):
            version_comparison.compare_bin_versions("8.3", "8")

    def test_fcv_style_versions(self):
        """Typical FCV values used in remove_shard_util and add_remove_shards."""
        self.assertGreaterEqual(version_comparison.compare_bin_versions("8.3", "8.3"), 0)
        self.assertGreaterEqual(version_comparison.compare_bin_versions("8.3.0", "8.3"), 0)
        self.assertLess(version_comparison.compare_bin_versions("8.2", "8.3"), 0)
        self.assertGreater(version_comparison.compare_bin_versions("8.4", "8.3"), 0)
