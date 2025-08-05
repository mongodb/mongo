"""Unit tests for Evergreen expansion extensions_required."""
import unittest

from buildscripts.ciconfig.evergreen import parse_evergreen_file


class TestEvergreenExtensionsRequired(unittest.TestCase):
    """Test that extensions_required=true is only set on Linux distros."""

    def setUp(self):
        """Parse the evergreen project file."""
        self.evg_config = parse_evergreen_file("etc/evergreen.yml")

    def test_extensions_required_expansion(self):
        """
        Test that variants with extensions_required=true are Linux.
        """
        for variant in self.evg_config.variants:
            if variant.expansion("extensions_required") == "true":
                for distro in variant.distro_names:
                    is_linux = ("linux" in distro or "ubuntu" in distro or "rhel" in distro or
                                "debian" in distro or "amazon" in distro)
                    self.assertTrue(
                        is_linux,
                        f"extensions_required can only be set to true on Linux distros. Variant {variant.name} has extensions_required=true but runs on non-Linux distro {distro}."
                    )


if __name__ == "__main__":
    unittest.main()
