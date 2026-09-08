"""Unit tests for debugsymb_mapper.py."""

import os
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import MagicMock, patch

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
            "    Applies to region from 0xb71 to 0xb71 (.annobin_init.c.hot)"
        )
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
            "Displaying notes found in: .gnu.build.attributes.hot"
        )
        self.cmd_client_mock.run.return_value = readelf_output

        self.assertRaises(ValueError, self.cmd_output_extractor.get_build_id, "path/to/bin")

    def test_get_build_id_returns_none(self):
        readelf_output = (
            "Displaying notes found in: .note.gnu.build-id\n"
            "  Owner                 Data size\tDescription\n"
            "  GNU                  0x00000014\tNT_GNU_BUILD_ID (unique build ID bitstring)"
        )
        self.cmd_client_mock.run.return_value = readelf_output

        build_id_output = self.cmd_output_extractor.get_build_id("path/to/bin")
        self.assertIsNone(build_id_output.build_id)
        self.assertEqual(build_id_output.cmd_output, readelf_output)


class TestGetBinVersion(TestCmdOutputExtractor):
    def test_get_bin_version_returns_version(self):
        # Newer versions command output
        version_cmd_output = (
            "db version v4.4.14-25-gb0475e2\n"
            "Build Info: {\n"
            '    "version": "4.4.14-25-gb0475e2",\n'
            '    "gitVersion": "b0475e2657c3351b25499971d3340f054ea85b98",\n'
            '    "openSSLVersion": "OpenSSL 1.1.1  11 Sep 2018",\n'
            '    "modules": [\n'
            '        "enterprise"\n'
            "    ],\n"
            '    "allocator": "tcmalloc",\n'
            '    "environment": {\n'
            '        "distmod": "ubuntu1804",\n'
            '        "distarch": "x86_64",\n'
            '        "target_arch": "x86_64"\n'
            "    }\n"
            "}"
        )
        self.cmd_client_mock.run.return_value = version_cmd_output

        bin_version_output = self.cmd_output_extractor.get_bin_version("path/to/bin")
        self.assertEqual(bin_version_output.mongodb_version, "4.4.14-25-gb0475e2")
        self.assertEqual(bin_version_output.cmd_output, version_cmd_output)

    def test_get_bin_version_unsupported_output(self):
        # Versions prior to 5.0 are not supported
        version_cmd_output = (
            "db version v4.2.20-7-g5a81409\n"
            "git version: 5a81409faf16f30f1189af6367eb3ceee50a02b5\n"
            "OpenSSL version: OpenSSL 1.1.1  11 Sep 2018\n"
            "allocator: tcmalloc\n"
            "modules: enterprise \n"
            "build environment:\n"
            "    distmod: ubuntu1804\n"
            "    distarch: x86_64\n"
            "    target_arch: x86_64"
        )
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


class TestSetupUrls(unittest.TestCase):
    def _make_mapper(self, is_san_variant=False):
        mapper = under_test.Mapper.__new__(under_test.Mapper)
        mapper.evg_version = "version_id"
        mapper.evg_variant = "variant"
        mapper.is_san_variant = is_san_variant
        mapper.logger = MagicMock()
        mapper.multiversion_setup = MagicMock()
        mapper.num_url_retries = 3
        mapper.url_retry_initial_delay_secs = 15
        mapper.url_retry_max_delay_secs = 120
        return mapper

    def _urlinfo(self, urls):
        urlinfo = MagicMock()
        urlinfo.urls = urls
        return urlinfo

    def test_setup_urls_success_without_retry(self):
        mapper = self._make_mapper()
        mapper.multiversion_setup.get_urls.return_value = self._urlinfo(
            {"Binaries": "binaries_url", "mongo-debugsymbols.tgz": "symbols_url"}
        )

        mapper.setup_urls()

        self.assertEqual(mapper.url, "binaries_url")
        self.assertEqual(mapper.debug_symbols_url, "symbols_url")
        mapper.multiversion_setup.get_urls.assert_called_once()

    def test_setup_urls_retries_when_artifacts_are_missing(self):
        mapper = self._make_mapper()
        # Simulate Evergreen's secondary node returning a stale, partial
        # artifact list before replication catches up.
        mapper.multiversion_setup.get_urls.side_effect = [
            self._urlinfo({"Pip Requirements": "pip_url"}),
            self._urlinfo({"Binaries": "binaries_url"}),
            self._urlinfo({"Binaries": "binaries_url", "mongo-debugsymbols.tgz": "symbols_url"}),
        ]

        with patch.object(under_test.time, "sleep") as sleep_mock:
            mapper.setup_urls()

        self.assertEqual(mapper.url, "binaries_url")
        self.assertEqual(mapper.debug_symbols_url, "symbols_url")
        self.assertEqual(mapper.multiversion_setup.get_urls.call_count, 3)
        self.assertEqual(sleep_mock.call_count, 2)

    def test_setup_urls_raises_after_exhausting_retries(self):
        mapper = self._make_mapper()
        mapper.multiversion_setup.get_urls.return_value = self._urlinfo(
            {"Pip Requirements": "pip_url"}
        )

        with patch.object(under_test.time, "sleep"):
            self.assertRaises(ValueError, mapper.setup_urls)

        self.assertEqual(mapper.multiversion_setup.get_urls.call_count, 3)

    def test_setup_urls_san_variant_uses_binaries_for_symbols(self):
        mapper = self._make_mapper(is_san_variant=True)
        mapper.multiversion_setup.get_urls.return_value = self._urlinfo(
            {"Binaries": "binaries_url"}
        )

        mapper.setup_urls()

        self.assertEqual(mapper.url, "binaries_url")
        self.assertEqual(mapper.debug_symbols_url, "binaries_url")

    def test_setup_urls_san_variant_retries_when_binaries_missing(self):
        mapper = self._make_mapper(is_san_variant=True)
        mapper.multiversion_setup.get_urls.side_effect = [
            self._urlinfo({"mongo-debugsymbols.tgz": "symbols_url"}),
            self._urlinfo({"Binaries": "binaries_url", "mongo-debugsymbols.tgz": "symbols_url"}),
        ]

        with patch.object(under_test.time, "sleep"):
            mapper.setup_urls()

        self.assertEqual(mapper.url, "binaries_url")
        self.assertEqual(mapper.debug_symbols_url, "binaries_url")
        self.assertEqual(mapper.multiversion_setup.get_urls.call_count, 2)


class TestGenerateLocalBuildIdMapping(unittest.TestCase):
    """Tests for the local-directory mode used by the resmoke_tests task."""

    def setUp(self):
        self.tmp_dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp_dir.cleanup)
        self.binaries_dir = self.tmp_dir.name
        self.bin_dir = os.path.join(self.binaries_dir, "bin")
        self.lib_dir = os.path.join(self.binaries_dir, "lib")
        os.makedirs(self.bin_dir)
        os.makedirs(self.lib_dir)

    def _touch(self, directory, name):
        with open(os.path.join(directory, name), "w") as file_handle:
            file_handle.write("")

    def _make_mapper(self, mongodb_version=None):
        mapper = under_test.Mapper.__new__(under_test.Mapper)
        mapper.logger = MagicMock()
        mapper.path_options = under_test.PathOptions()
        mapper.binaries_dir = self.binaries_dir
        mapper.mongodb_version = mongodb_version
        mapper.url = "binaries_url"
        mapper.debug_symbols_url = "binaries_url"
        mapper.extractor = MagicMock(spec_set=under_test.CmdOutputExtractor)
        mapper.extractor.get_build_id.side_effect = lambda path: under_test.BuildIdOutput(
            f"buildid-{os.path.basename(path)}", "readelf output"
        )
        mapper.extractor.get_bin_version.return_value = under_test.BinVersionOutput(
            "8.3.0-alpha", "version output"
        )
        return mapper

    def test_maps_binaries_and_shared_libraries(self):
        self._touch(self.bin_dir, "mongod")
        self._touch(self.bin_dir, "mongos")
        self._touch(self.lib_dir, "libfoo.so")
        mapper = self._make_mapper(mongodb_version="8.3.0-alpha")

        mappings = list(mapper.generate_local_build_id_mapping())

        self.assertEqual(
            [mapping["file_name"] for mapping in mappings], ["mongod", "mongos", "libfoo.so"]
        )
        for mapping in mappings:
            self.assertEqual(mapping["url"], "binaries_url")
            self.assertEqual(mapping["debug_symbols_url"], "binaries_url")
            self.assertEqual(mapping["version"], "8.3.0-alpha")
            self.assertEqual(mapping["build_id"], f"buildid-{mapping['file_name']}")

    def test_skips_debug_info_files(self):
        # The relinked tree holds both the stripped binary and its symbol container. Only the
        # loadable object gets a mapping; the .debug file is what debug_symbols_url points at.
        self._touch(self.bin_dir, "mongod")
        self._touch(self.bin_dir, "mongod.debug")
        self._touch(self.bin_dir, "mongod.dwp")
        self._touch(self.lib_dir, "libfoo.so")
        self._touch(self.lib_dir, "libfoo.so.debug")
        mapper = self._make_mapper(mongodb_version="8.3.0-alpha")

        mappings = list(mapper.generate_local_build_id_mapping())

        self.assertEqual([mapping["file_name"] for mapping in mappings], ["mongod", "libfoo.so"])

    def test_falls_back_to_passed_in_version_when_mongod_is_absent(self):
        # A failed suite's relink only produces the binaries that suite needed, so mongod is
        # not necessarily in the tree.
        self._touch(self.bin_dir, "mongos")
        mapper = self._make_mapper(mongodb_version="8.3.0-alpha")

        mappings = list(mapper.generate_local_build_id_mapping())

        self.assertEqual([mapping["file_name"] for mapping in mappings], ["mongos"])
        self.assertEqual(mappings[0]["version"], "8.3.0-alpha")
        mapper.extractor.get_bin_version.assert_not_called()

    def test_prefers_version_read_off_mongod(self):
        self._touch(self.bin_dir, "mongod")
        mapper = self._make_mapper(mongodb_version="fallback-version")
        mapper.extractor.get_bin_version.return_value = under_test.BinVersionOutput(
            "8.3.0-alpha", "version output"
        )

        mappings = list(mapper.generate_local_build_id_mapping())

        self.assertEqual(mappings[0]["version"], "8.3.0-alpha")

    def test_falls_back_when_mongod_version_is_unreadable(self):
        self._touch(self.bin_dir, "mongod")
        mapper = self._make_mapper(mongodb_version="fallback-version")
        mapper.extractor.get_bin_version.return_value = under_test.BinVersionOutput(
            None, "error output"
        )

        mappings = list(mapper.generate_local_build_id_mapping())

        self.assertEqual(mappings[0]["version"], "fallback-version")

    def test_yields_nothing_without_a_version(self):
        self._touch(self.bin_dir, "mongos")
        mapper = self._make_mapper(mongodb_version=None)

        self.assertEqual(list(mapper.generate_local_build_id_mapping()), [])

    def test_skips_binaries_without_a_build_id(self):
        self._touch(self.bin_dir, "mongod")
        self._touch(self.bin_dir, "mongos")
        mapper = self._make_mapper(mongodb_version="8.3.0-alpha")
        mapper.extractor.get_build_id.side_effect = lambda path: under_test.BuildIdOutput(
            None if path.endswith("mongos") else "buildid-mongod", "readelf output"
        )

        mappings = list(mapper.generate_local_build_id_mapping())

        self.assertEqual([mapping["file_name"] for mapping in mappings], ["mongod"])

    def test_tolerates_a_missing_lib_folder(self):
        os.rmdir(self.lib_dir)
        self._touch(self.bin_dir, "mongod")
        mapper = self._make_mapper(mongodb_version="8.3.0-alpha")

        mappings = list(mapper.generate_local_build_id_mapping())

        self.assertEqual([mapping["file_name"] for mapping in mappings], ["mongod"])


class TestLocalModeSetup(unittest.TestCase):
    """The local mode must not reach for compile task artifacts."""

    def _init_kwargs(self, **overrides):
        kwargs = {
            "evg_version": "version_id",
            "evg_variant": "variant",
            "is_san_variant": False,
            "client_id": "client_id",
            "client_secret": "client_secret",
            "cache_dir": None,
            "logger": MagicMock(),
        }
        kwargs.update(overrides)
        return kwargs

    def _make_evg_api(self, artifacts):
        task = MagicMock()
        task.artifacts = artifacts
        evg_api = MagicMock()
        evg_api.task_by_id.return_value = task
        return evg_api

    def _artifact(self, name, url):
        return SimpleNamespace(name=name, url=url)

    def test_local_mode_skips_artifact_discovery(self):
        with tempfile.TemporaryDirectory() as cache_dir:
            with (
                patch.object(under_test.Mapper, "authenticate"),
                patch.object(under_test, "SetupMultiversion") as setup_multiversion_mock,
                patch.object(under_test.Mapper, "setup_urls") as setup_urls_mock,
            ):
                mapper = under_test.Mapper(
                    **self._init_kwargs(
                        cache_dir=cache_dir,
                        binaries_dir="dist-tests",
                        binaries_url="tests_url",
                    )
                )

            setup_multiversion_mock.assert_not_called()
            setup_urls_mock.assert_not_called()
            self.assertEqual(mapper.url, "tests_url")
            # One archive holds the binaries and their .debug files.
            self.assertEqual(mapper.debug_symbols_url, "tests_url")

    def test_local_mode_honours_an_explicit_debug_symbols_url(self):
        with tempfile.TemporaryDirectory() as cache_dir:
            with (
                patch.object(under_test.Mapper, "authenticate"),
                patch.object(under_test, "SetupMultiversion"),
                patch.object(under_test.Mapper, "setup_urls"),
                patch.object(under_test.evergreen_conn, "get_evergreen_api") as get_api_mock,
            ):
                mapper = under_test.Mapper(
                    **self._init_kwargs(
                        cache_dir=cache_dir,
                        binaries_dir="dist-tests",
                        binaries_url="tests_url",
                        debug_symbols_url="symbols_url",
                    )
                )

            get_api_mock.assert_not_called()
            self.assertEqual(mapper.url, "tests_url")
            self.assertEqual(mapper.debug_symbols_url, "symbols_url")

    def test_local_mode_resolves_task_artifact_url(self):
        # Raw mciuploads URLs of the private archive 403 for symbolizer clients; the Evergreen
        # API artifact URL is what gets recorded (see evergreen/spawnhost/download_test_binaries.py).
        evg_api = self._make_evg_api(
            [
                self._artifact("Test binaries and libraries - Execution 0", "artifact_url"),
            ]
        )
        with tempfile.TemporaryDirectory() as cache_dir:
            with (
                patch.object(under_test.Mapper, "authenticate"),
                patch.object(under_test, "SetupMultiversion"),
                patch.object(under_test.Mapper, "setup_urls"),
                patch.object(under_test.evergreen_conn, "get_evergreen_api", return_value=evg_api),
            ):
                mapper = under_test.Mapper(
                    **self._init_kwargs(
                        cache_dir=cache_dir, binaries_dir="dist-tests", task_id="task_id"
                    )
                )

            evg_api.task_by_id.assert_called_once_with("task_id")
            self.assertEqual(mapper.url, "artifact_url")
            self.assertEqual(mapper.debug_symbols_url, "artifact_url")

    def test_local_mode_retries_when_artifact_list_lags(self):
        # The Evergreen API's secondary node can lag behind a just-attached artifact
        # (DEVPROD-32223), so the resolution retries before giving up.
        empty_task = MagicMock()
        empty_task.artifacts = []
        task_with_artifact = MagicMock()
        task_with_artifact.artifacts = [
            self._artifact("Test binaries and libraries - Execution 0", "artifact_url")
        ]
        evg_api = MagicMock()
        evg_api.task_by_id.side_effect = [empty_task, task_with_artifact]

        with tempfile.TemporaryDirectory() as cache_dir:
            with (
                patch.object(under_test.Mapper, "authenticate"),
                patch.object(under_test, "SetupMultiversion"),
                patch.object(under_test.Mapper, "setup_urls"),
                patch.object(under_test.evergreen_conn, "get_evergreen_api", return_value=evg_api),
                patch.object(under_test.time, "sleep") as sleep_mock,
            ):
                mapper = under_test.Mapper(
                    **self._init_kwargs(
                        cache_dir=cache_dir, binaries_dir="dist-tests", task_id="task_id"
                    )
                )

            self.assertEqual(evg_api.task_by_id.call_count, 2)
            sleep_mock.assert_called_once()
            self.assertEqual(mapper.url, "artifact_url")

    def test_local_mode_requires_a_binaries_url_or_task_id(self):
        with tempfile.TemporaryDirectory() as cache_dir:
            with (
                patch.object(under_test.Mapper, "authenticate"),
                patch.object(under_test, "SetupMultiversion"),
                patch.object(under_test.Mapper, "setup_urls"),
            ):
                with self.assertRaises(ValueError):
                    under_test.Mapper(
                        **self._init_kwargs(cache_dir=cache_dir, binaries_dir="dist-tests")
                    )

    def test_local_mode_task_id_resolution_raises_when_artifact_missing(self):
        evg_api = self._make_evg_api([])
        with tempfile.TemporaryDirectory() as cache_dir:
            with (
                patch.object(under_test.Mapper, "authenticate"),
                patch.object(under_test, "SetupMultiversion"),
                patch.object(under_test.Mapper, "setup_urls"),
                patch.object(under_test.evergreen_conn, "get_evergreen_api", return_value=evg_api),
                patch.object(under_test.time, "sleep"),
            ):
                with self.assertRaises(ValueError):
                    under_test.Mapper(
                        **self._init_kwargs(
                            cache_dir=cache_dir, binaries_dir="dist-tests", task_id="task_id"
                        )
                    )

    def test_compile_task_mode_still_discovers_artifacts(self):
        with tempfile.TemporaryDirectory() as cache_dir:
            with (
                patch.object(under_test.Mapper, "authenticate"),
                patch.object(under_test, "SetupMultiversion") as setup_multiversion_mock,
                patch.object(under_test.Mapper, "setup_urls") as setup_urls_mock,
            ):
                under_test.Mapper(**self._init_kwargs(cache_dir=cache_dir))

            setup_multiversion_mock.assert_called_once()
            setup_urls_mock.assert_called_once()


if __name__ == "__main__":
    unittest.main()
