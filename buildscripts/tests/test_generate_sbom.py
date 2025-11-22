import logging
import sys
import unittest

sys.path.append(".")

from buildscripts.sbom.config import (
    get_semver_from_release_version,
    is_valid_purl,
    regex_semver,
)
from buildscripts.sbom.endorctl_utils import EndorCtl

logging.basicConfig(level=logging.INFO, stream=sys.stdout)


class TestEndorctl(unittest.TestCase):
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
        result = e.get_sbom_for_project("https://github.com/10gen/mongo.git")
        self.assertRaises(FileNotFoundError)
        self.assertIsNone(result, None)


class TestConfigRegex(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main(verbosity=2)
