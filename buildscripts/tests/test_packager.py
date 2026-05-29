"""Unit tests for the packager script."""

import sys
from dataclasses import dataclass
from pathlib import Path
from unittest import TestCase

sys.path.append(str(Path(__file__).parents[1]))

from buildscripts.packager import Distro, Spec
from buildscripts.packager_enterprise import EnterpriseDistro


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

    def test_rhel10_major_version_parsing(self) -> None:
        """Test Red Hat major versions used by repo paths and RPM dist tags."""
        distro = Distro("redhat")

        self.assertEqual(distro.repo_os_version("rhel88"), "8")
        self.assertEqual(distro.release_dist("rhel88"), "el8")
        self.assertEqual(distro.repo_os_version("rhel93"), "9")
        self.assertEqual(distro.release_dist("rhel93"), "el9")
        self.assertEqual(distro.repo_os_version("rhel10"), "10")
        self.assertEqual(distro.release_dist("rhel10"), "el10")
        self.assertEqual(distro.repo_os_version("rhel100"), "10")
        self.assertEqual(distro.release_dist("rhel100"), "el10")

    def test_rhel10_community_build_os(self) -> None:
        """Test community packaging accepts RHEL10 build OS labels."""
        distro = Distro("redhat")

        self.assertIn("rhel10", distro.build_os("x86_64"))
        self.assertIn("rhel10", distro.build_os("aarch64"))

    def test_rhel10_enterprise_build_os(self) -> None:
        """Test enterprise packaging accepts RHEL10 build OS labels."""
        distro = EnterpriseDistro("redhat")

        self.assertIn("rhel10", distro.build_os("x86_64"))
        self.assertIn("rhel10", distro.build_os("aarch64"))
