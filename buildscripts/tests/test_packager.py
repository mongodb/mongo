"""Unit tests for the packager script."""
import sys
from dataclasses import dataclass
from pathlib import Path
from unittest import TestCase

sys.path.append(str(Path(__file__).parents[1]))

# pylint: disable=wrong-import-position
from buildscripts.packager import Distro, Spec
from buildscripts.packager_enterprise import EnterpriseDistro

# pylint: enable=wrong-import-position


class TestPackager(TestCase):
    """Test packager.py"""

    def test_is_nightly(self) -> None:
        """Test is_nightly."""

        @dataclass
        class Case:
            """Test case data"""
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

        self.assertEqual(distro.repo_os_version("rhel82"), "8")
        self.assertEqual(distro.release_dist("rhel82"), "el8")
        self.assertEqual(distro.repo_os_version("rhel90"), "9")
        self.assertEqual(distro.release_dist("rhel90"), "el9")
        self.assertEqual(distro.repo_os_version("rhel10"), "10")
        self.assertEqual(distro.release_dist("rhel10"), "el10")

    def test_rhel10_community_build_os(self) -> None:
        """Test community packaging accepts RHEL10 build OS labels."""
        distro = Distro("redhat")

        self.assertIn("rhel10", distro.build_os("x86_64"))
        self.assertIn("rhel10", distro.build_os("aarch64"))

    def test_redhat_release_dist(self) -> None:
        """Test the rpm release dist for RHEL, including multi-digit major versions."""
        distro = Distro("redhat")

        self.assertEqual("el5", distro.release_dist("rhel55"))
        self.assertEqual("el8", distro.release_dist("rhel80"))
        self.assertEqual("el9", distro.release_dist("rhel93"))
        self.assertEqual("el10", distro.release_dist("rhel10"))

    def test_redhat_repo_os_version(self) -> None:
        """Test the repo os version for RHEL, including multi-digit major versions."""
        distro = Distro("redhat")

        self.assertEqual("7", distro.repo_os_version("rhel79"))
        self.assertEqual("9", distro.repo_os_version("rhel93"))
        self.assertEqual("10", distro.repo_os_version("rhel10"))

    def test_redhat_release_dist_accepts_major_only(self) -> None:
        """Test an RHEL 9 and earlier name with no minor version."""
        self.assertEqual("el9", Distro("redhat").release_dist("rhel9"))

    def test_redhat_release_dist_rejects_unknown_build_os(self) -> None:
        """Test that an unrecognized RHEL build OS name is rejected rather than silently mangled.

        "rhel101" is not expected to ever be valid, but if the naming scheme does change
        it must be handled deliberately rather than truncated to some other version.
        """
        for build_os in ["rhelfoo", "rhel", "rhel1", "rhel101", "rhel9310", "rhel10.1"]:
            with self.subTest(build_os=build_os):
                with self.assertRaises(Exception):
                    Distro("redhat").release_dist(build_os)

    def test_redhat_all_build_os_names_are_parseable(self) -> None:
        """Test that every RHEL build OS we actually package for has a parseable name."""
        for distro in [Distro("redhat"), EnterpriseDistro("redhat")]:
            for arch in ["x86_64", "aarch64", "ppc64le", "s390x"]:
                for build_os in distro.build_os(arch):
                    with self.subTest(distro=type(distro).__name__, build_os=build_os):
                        self.assertTrue(distro.release_dist(build_os).startswith("el"))

    def test_rhel10_enterprise_build_os(self) -> None:
        """Test enterprise packaging accepts RHEL10 build OS labels."""
        distro = EnterpriseDistro("redhat")

        self.assertIn("rhel10", distro.build_os("x86_64"))
        self.assertIn("rhel10", distro.build_os("aarch64"))
