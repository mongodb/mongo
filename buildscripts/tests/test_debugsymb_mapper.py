"""Unit tests for debugsymb_mapper.py."""

import unittest
from unittest.mock import MagicMock

import buildscripts.debugsymb_mapper as under_test


def mock_cmd_client():
    cmd_client = MagicMock(spec_set=under_test.CmdClient)
    return cmd_client


class TestCmdOutputExtractor(unittest.TestCase):
    def setUp(self):
        self.cmd_client_mock = mock_cmd_client()
        self.cmd_output_extractor = under_test.CmdOutputExtractor(self.cmd_client_mock)


class TestGetBuildId(TestCmdOutputExtractor):
    def test_get_build_id_returns_build_id(self):
        readelf_output = (
            "Displaying notes found in: .note.gnu.build-id\n"
            "  Owner                 Data size\tDescription\n"
            "  GNU                  0x00000014\tNT_GNU_BUILD_ID (unique build ID bitstring)\n"
            "    Build ID: 74c2322104428836f3d94af6cd7471ee7cb5c4ee\n"
            "\n"
            "Displaying notes found in: .gnu.build.attributes.hot\n"
            "  Owner                 Data size\tDescription\n"
            "  GA$<version>3h864    0x00000010\tOPEN\n"
            "    Applies to region from 0xb71 to 0xb71 (.annobin_init.c.hot)\n"
            "  GA$<version>3h864    0x00000010\tOPEN\n"
            "    Applies to region from 0xb71 to 0xb71 (.annobin_init.c.hot)")
        self.cmd_client_mock.run.return_value = readelf_output

        build_id_output = self.cmd_output_extractor.get_build_id("path/to/bin")
        self.assertEqual(build_id_output.build_id, "74c2322104428836f3d94af6cd7471ee7cb5c4ee")
        self.assertEqual(build_id_output.cmd_output, readelf_output)

    def test_get_build_id_raises_error(self):
        readelf_output = (
            "  Owner                 Data size\tDescription\n"
            "  GNU                  0x00000014\tNT_GNU_BUILD_ID (unique build ID bitstring)\n"
            "    Build ID: 74c2322104428836f3d94af6cd7471ee7cb5c4ee\n"
            "\n"
            "Displaying notes found in: .gnu.build.attributes.hot\n"
            "  Owner                 Data size\tDescription\n"
            "  GNU                  0x00000014\tNT_GNU_BUILD_ID (unique build ID bitstring)\n"
            "    Build ID: 74c2322104428836f3d94af6cd7471ee7cb5c4ee\n"
            "\n"
            "Displaying notes found in: .gnu.build.attributes.hot")
        self.cmd_client_mock.run.return_value = readelf_output

        self.assertRaises(ValueError, self.cmd_output_extractor.get_build_id, "path/to/bin")

    def test_get_build_id_returns_none(self):
        readelf_output = (
            "Displaying notes found in: .note.gnu.build-id\n"
            "  Owner                 Data size\tDescription\n"
            "  GNU                  0x00000014\tNT_GNU_BUILD_ID (unique build ID bitstring)")
        self.cmd_client_mock.run.return_value = readelf_output

        build_id_output = self.cmd_output_extractor.get_build_id("path/to/bin")
        self.assertIsNone(build_id_output.build_id)
        self.assertEqual(build_id_output.cmd_output, readelf_output)


class TestGetBinVersion(TestCmdOutputExtractor):
    def test_get_bin_version_returns_version(self):
        # Newer versions command output
        version_cmd_output = ('db version v4.4.14-25-gb0475e2\n'
                              'Build Info: {\n'
                              '    "version": "4.4.14-25-gb0475e2",\n'
                              '    "gitVersion": "b0475e2657c3351b25499971d3340f054ea85b98",\n'
                              '    "openSSLVersion": "OpenSSL 1.1.1  11 Sep 2018",\n'
                              '    "modules": [\n'
                              '        "enterprise"\n'
                              '    ],\n'
                              '    "allocator": "tcmalloc",\n'
                              '    "environment": {\n'
                              '        "distmod": "ubuntu1804",\n'
                              '        "distarch": "x86_64",\n'
                              '        "target_arch": "x86_64"\n'
                              '    }\n'
                              '}')
        self.cmd_client_mock.run.return_value = version_cmd_output

        bin_version_output = self.cmd_output_extractor.get_bin_version("path/to/bin")
        self.assertEqual(bin_version_output.mongodb_version, "4.4.14-25-gb0475e2")
        self.assertEqual(bin_version_output.cmd_output, version_cmd_output)

    def test_get_bin_version_unsupported_output(self):
        # Versions prior to 5.0 are not supported
        version_cmd_output = ('db version v4.2.20-7-g5a81409\n'
                              'git version: 5a81409faf16f30f1189af6367eb3ceee50a02b5\n'
                              'OpenSSL version: OpenSSL 1.1.1  11 Sep 2018\n'
                              'allocator: tcmalloc\n'
                              'modules: enterprise \n'
                              'build environment:\n'
                              '    distmod: ubuntu1804\n'
                              '    distarch: x86_64\n'
                              '    target_arch: x86_64')
        self.cmd_client_mock.run.return_value = version_cmd_output

        bin_version_output = self.cmd_output_extractor.get_bin_version("path/to/bin")
        self.assertIsNone(bin_version_output.mongodb_version)
        self.assertEqual(bin_version_output.cmd_output, version_cmd_output)

    def test_get_bin_version_returns_none(self):
        version_cmd_output = "error: unrecognized arguments: --version"
        self.cmd_client_mock.run.return_value = version_cmd_output

        bin_version_output = self.cmd_output_extractor.get_bin_version("path/to/bin")
        self.assertIsNone(bin_version_output.mongodb_version)
        self.assertEqual(bin_version_output.cmd_output, version_cmd_output)
