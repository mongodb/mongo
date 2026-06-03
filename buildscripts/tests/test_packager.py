"""Unit tests for the packager script."""

import unittest
from dataclasses import dataclass
from unittest import TestCase

from buildscripts.packager import Distro, Spec


class TestPackager(TestCase):
    """Test packager.py."""

    def test_is_nightly(self) -> None:
        """Test is_nightly."""

        @dataclass
        class Case:
            """Test case data."""

            name: str
            version: str
            want: bool

        cases = [
            Case(
                name="Waterfall alpha",
                version="5.3.0-alpha-211-g546d77f",
                want=True,
            ),
            Case(
                name="Waterfall",
                version="5.3.0-211-g546d77f",
                want=True,
            ),
            Case(
                name="Mainline",
                version="5.3.0",
                want=False,
            ),
            Case(
                name="Release candidate",
                version="5.3.0-rc0",
                want=False,
            ),
        ]

        for case in cases:
            with self.subTest(name=case.name):
                spec = Spec(ver=case.version)
                self.assertEqual(spec.is_nightly(), case.want)

    def test_community_suffix(self) -> None:
        """Test community suffix"""

        @dataclass
        class Case:
            """Test case data."""

            name: str
            version: str
            want: str

        cases = [
            Case(name="Old unstable", version="4.3.0", want="-org-unstable"),
            Case(name="Old stable 4.2", version="4.2.0", want="-org"),
            Case(name="Old stable 4.4", version="4.4.0", want="-org"),
            Case(
                name="New stable standard",
                version="8.0.0",
                want="-org",
            ),
            Case(
                name="New unstable standard 8.1",
                version="8.1.0",
                want="-org-unstable",
            ),
            Case(
                name="New unstable standard 7.2",
                version="7.2.0",
                want="-org-unstable",
            ),
            Case(
                name="New stable special case",
                version="8.2.0",
                want="-org",
            ),
        ]

        for case in cases:
            with self.subTest(name=case.name):
                spec = Spec(ver=case.version)
                self.assertEqual(spec.suffix(), case.want)

    def test_debian13_repo_os_version(self) -> None:
        """Test Debian 13 repository codename."""
        self.assertEqual("trixie", Distro("debian").repo_os_version("debian13"))

    def test_debian_build_os_includes_debian13(self) -> None:
        """Test Debian build OS choices include Debian 13."""
        self.assertIn("debian13", Distro("debian").build_os("x86_64"))

    def test_suse16_repo_os_version(self) -> None:
        """Test SUSE 16 repository version."""
        self.assertEqual("16", Distro("suse").repo_os_version("suse16"))

    def test_suse_build_os_includes_suse16(self) -> None:
        """Test SUSE build OS choices include SUSE 16."""
        self.assertIn("suse16", Distro("suse").build_os("x86_64"))


if __name__ == "__main__":
    unittest.main()
