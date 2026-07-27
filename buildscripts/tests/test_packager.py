"""Unit tests for the packager script."""

import unittest
from dataclasses import dataclass
from unittest import TestCase

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

    def test_redhat_release_dist_accepts_major_only_legacy_name(self) -> None:
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

    def test_enterprise_rhel10_includes_ibm_architectures(self) -> None:
        """Test Enterprise RHEL 10 build OS choices include IBM architectures."""
        distro = EnterpriseDistro("redhat")

        self.assertIn("rhel10", distro.build_os("ppc64le"))
        self.assertIn("rhel10", distro.build_os("s390x"))


if __name__ == "__main__":
    unittest.main()
