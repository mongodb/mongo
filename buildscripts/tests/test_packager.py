"""Unit tests for the packager script."""
from dataclasses import dataclass
from unittest import TestCase

from buildscripts.packager import Spec


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
