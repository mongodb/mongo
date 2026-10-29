"""Unit tests for buildscripts.resmokelib.extensions.find_and_generate_extension_configs."""

import hashlib
import io
import logging
import os
import platform
import tarfile
import tempfile
import unittest
from unittest.mock import patch

import yaml

from buildscripts.resmokelib.extensions.delete_extension_configs import delete_extension_configs
from buildscripts.resmokelib.extensions.download_external_extensions import (
    download_external_extension,
)
from buildscripts.resmokelib.extensions.find_and_generate_extension_configs import (
    build_mongot_dynamic_options,
    find_and_generate_named_extension_configs,
    get_mongot_extension_name,
    mongot_extension_requested,
)

MONGOT_EXTENSION_NAME = get_mongot_extension_name()


class TestFindAndGenerateNamedExtensionConfigs(unittest.TestCase):
    """Tests for find_and_generate_named_extension_configs's extension-name resolution."""

    def setUp(self):
        self.tmp_dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp_dir.cleanup)
        self.ext_dir = self.tmp_dir.name
        self.logger = logging.getLogger("test_find_and_generate_extension_configs")
        get_extension_dir_patcher = patch(
            "buildscripts.resmokelib.extensions.find_and_generate_extension_configs._get_extension_dir",
            return_value=self.ext_dir,
        )
        get_extension_dir_patcher.start()
        self.addCleanup(get_extension_dir_patcher.stop)

        generate_configs_patcher = patch(
            "buildscripts.resmokelib.extensions.find_and_generate_extension_configs.generate_extension_configs",
            side_effect=lambda so_files, with_suffix, logger, manual_options_by_file=None: [
                f"{os.path.basename(f)}_{with_suffix}" for f in so_files
            ],
        )
        self.generate_configs_mock = generate_configs_patcher.start()
        self.addCleanup(generate_configs_patcher.stop)

    def _touch(self, filename: str) -> str:
        path = os.path.join(self.ext_dir, filename)
        open(path, "w").close()
        return path

    def test_finds_in_tree_test_extension_by_mongo_extension_suffix(self):
        so_path = self._touch("libadd_fields_match_mongo_extension.so")

        mongod_options = {}
        find_and_generate_named_extension_configs(
            extension_names=["add_fields_match"],
            is_evergreen=False,
            logger=self.logger,
            mongod_options=mongod_options,
        )

        args, _kwargs = self.generate_configs_mock.call_args
        self.assertEqual(args[0], [so_path])

    def test_finds_external_extension_without_mongo_infix(self):
        # mongot-extension / rerank-extension are published as lib<name>_extension.so, not
        # lib<name>_mongo_extension.so.
        so_path = self._touch("libmongot_extension.so")

        mongod_options = {}
        find_and_generate_named_extension_configs(
            extension_names=["mongot"],
            is_evergreen=False,
            logger=self.logger,
            mongod_options=mongod_options,
        )

        args, _kwargs = self.generate_configs_mock.call_args
        self.assertEqual(args[0], [so_path])

    def test_raises_when_extension_not_found(self):
        with self.assertRaisesRegex(RuntimeError, "not found"):
            find_and_generate_named_extension_configs(
                extension_names=["nonexistent"],
                is_evergreen=False,
                logger=self.logger,
                mongod_options={},
            )

    def test_raises_when_extension_name_ambiguous_across_conventions(self):
        # If both naming conventions somehow match the same extension name, that's ambiguous.
        self._touch("libfoo_mongo_extension.so")
        self._touch("libfoo_extension.so")

        with self.assertRaisesRegex(RuntimeError, "Ambiguous"):
            find_and_generate_named_extension_configs(
                extension_names=["foo"],
                is_evergreen=False,
                logger=self.logger,
                mongod_options={},
            )

    def test_glob_metacharacters_in_name_are_matched_literally(self):
        # A name containing glob metacharacters must not be interpreted as a wildcard.
        self._touch("libfoo_extension.so")

        with self.assertRaisesRegex(RuntimeError, "not found"):
            find_and_generate_named_extension_configs(
                extension_names=["f*o"],
                is_evergreen=False,
                logger=self.logger,
                mongod_options={},
            )


class TestDynamicExtensionOptions(unittest.TestCase):
    """Tests that dynamic_options reach the generated .conf, using the real (unmocked) pipeline."""

    def setUp(self):
        self.tmp_dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp_dir.cleanup)
        self.ext_dir = self.tmp_dir.name
        self.logger = logging.getLogger("test_dynamic_extension_options")

        get_extension_dir_patcher = patch(
            "buildscripts.resmokelib.extensions.find_and_generate_extension_configs._get_extension_dir",
            return_value=self.ext_dir,
        )
        get_extension_dir_patcher.start()
        self.addCleanup(get_extension_dir_patcher.stop)

        # Route generated .conf files into our own temp dir instead of the real TMPDIR. Both
        # modules import get_conf_out_dir by name, so each needs its own patch.
        self.conf_out_dir = os.path.join(self.tmp_dir.name, "conf_out")
        for target in (
            "buildscripts.resmokelib.extensions.generate_extension_configs.get_conf_out_dir",
            "buildscripts.resmokelib.extensions.delete_extension_configs.get_conf_out_dir",
            "buildscripts.resmokelib.extensions.find_and_generate_extension_configs.get_conf_out_dir",
        ):
            conf_out_dir_patcher = patch(target, return_value=self.conf_out_dir)
            conf_out_dir_patcher.start()
            self.addCleanup(conf_out_dir_patcher.stop)

    def _touch(self, filename: str) -> str:
        path = os.path.join(self.ext_dir, filename)
        open(path, "w").close()
        return path

    def _read_conf(self, generated_name: str) -> dict:
        with open(os.path.join(self.conf_out_dir, f"{generated_name}.conf")) as f:
            return yaml.safe_load(f)

    def test_dynamic_options_are_written_to_generated_conf(self):
        self._touch("libmongot_extension.so")

        mongod_options = {}
        find_and_generate_named_extension_configs(
            extension_names=[MONGOT_EXTENSION_NAME],
            is_evergreen=False,
            logger=self.logger,
            mongod_options=mongod_options,
            dynamic_options={MONGOT_EXTENSION_NAME: build_mongot_dynamic_options("localhost:1234")},
        )

        generated_names = mongod_options["loadExtensions"].split(",")
        self.assertEqual(len(generated_names), 1)
        conf_contents = self._read_conf(generated_names[0])
        self.assertEqual(conf_contents["extensionOptions"]["mongotHost"], "localhost:1234")

    def test_dynamic_options_only_applied_to_matching_extension(self):
        self._touch("libadd_fields_match_mongo_extension.so")
        self._touch("libmongot_extension.so")

        mongod_options = {}
        find_and_generate_named_extension_configs(
            extension_names=["add_fields_match", MONGOT_EXTENSION_NAME],
            is_evergreen=False,
            logger=self.logger,
            mongod_options=mongod_options,
            dynamic_options={MONGOT_EXTENSION_NAME: build_mongot_dynamic_options("localhost:1234")},
        )

        generated_names = mongod_options["loadExtensions"].split(",")
        self.assertEqual(len(generated_names), 2)

        add_fields_name = next(n for n in generated_names if n.startswith("add_fields_match_"))
        # No configurations.yml entry and no dynamic_options entry for add_fields_match: its conf
        # should have no extensionOptions at all.
        self.assertNotIn("extensionOptions", self._read_conf(add_fields_name) or {})

        mongot_name = next(n for n in generated_names if n.startswith(f"{MONGOT_EXTENSION_NAME}_"))
        self.assertEqual(
            self._read_conf(mongot_name)["extensionOptions"]["mongotHost"], "localhost:1234"
        )

    def _generate_mongot_conf(self, mongot_host: str) -> str:
        """Generate a mongot conf via the real pipeline; return its generated name."""
        self._touch("libmongot_extension.so")
        mongod_options: dict = {}
        generated_name = find_and_generate_named_extension_configs(
            extension_names=[MONGOT_EXTENSION_NAME],
            is_evergreen=False,
            logger=self.logger,
            mongod_options=mongod_options,
            dynamic_options={MONGOT_EXTENSION_NAME: build_mongot_dynamic_options(mongot_host)},
        )
        self.assertEqual(mongod_options["loadExtensions"], generated_name)
        return generated_name

    def test_delete_extension_configs_removes_conf_and_generated_metrics_file(self):
        generated_name = self._generate_mongot_conf("localhost:0")
        conf_path = os.path.join(self.conf_out_dir, f"{generated_name}.conf")
        metrics_path = self._read_conf(generated_name)["extensionOptions"]["metricsFilePath"]
        # Simulate the extension having created its metrics file at runtime.
        open(metrics_path, "w").close()

        delete_extension_configs(generated_name, self.logger)

        self.assertFalse(os.path.exists(conf_path))
        self.assertFalse(os.path.exists(metrics_path))

    def test_delete_extension_configs_leaves_external_metrics_file_alone(self):
        external_metrics = os.path.join(self.tmp_dir.name, "external.prom")
        open(external_metrics, "w").close()
        os.makedirs(self.conf_out_dir, exist_ok=True)
        with open(os.path.join(self.conf_out_dir, "custom.conf"), "w") as f:
            yaml.dump(
                {
                    "sharedLibraryPath": "/x/lib.so",
                    "extensionOptions": {"metricsFilePath": external_metrics},
                },
                f,
            )

        delete_extension_configs("custom", self.logger)

        # The conf is removed, but a metrics path outside the conf dir is never touched.
        self.assertFalse(os.path.exists(os.path.join(self.conf_out_dir, "custom.conf")))
        self.assertTrue(os.path.exists(external_metrics))


class TestMongotExtensionRequested(unittest.TestCase):
    """Tests for the mongot/launch_mongot validation helper."""

    def test_false_when_mongot_not_requested(self):
        self.assertFalse(mongot_extension_requested(["add_fields_match"], launch_mongot=False))
        self.assertFalse(mongot_extension_requested([], launch_mongot=True))

    def test_true_when_mongot_requested_with_launch_mongot(self):
        self.assertTrue(
            mongot_extension_requested(
                ["add_fields_match", MONGOT_EXTENSION_NAME], launch_mongot=True
            )
        )

    def test_raises_when_mongot_requested_without_launch_mongot(self):
        with self.assertRaisesRegex(RuntimeError, "launch_mongot"):
            mongot_extension_requested([MONGOT_EXTENSION_NAME], launch_mongot=False)


class TestDownloadExternalExtension(unittest.TestCase):
    """Tests for the local-run download fallback for externally-published extensions."""

    def setUp(self):
        self.tmp_dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp_dir.cleanup)
        self.logger = logging.getLogger("test_download_external_extension")

        self.conf_path = os.path.join(self.tmp_dir.name, "extensions.yml")
        self.cache_dir = os.path.join(self.tmp_dir.name, "cache")
        for target, value in (
            (
                "buildscripts.resmokelib.extensions.download_external_extensions.EXTERNAL_EXTENSIONS_CONF_PATH",
                self.conf_path,
            ),
            (
                "buildscripts.resmokelib.extensions.download_external_extensions.EXTERNAL_EXTENSIONS_CACHE_DIR",
                self.cache_dir,
            ),
        ):
            patcher = patch(target, value)
            patcher.start()
            self.addCleanup(patcher.stop)

    def _write_conf(self, checksum: str, variant: str = f"amazon2023-{platform.machine()}"):
        with open(self.conf_path, "w") as f:
            yaml.dump(
                {
                    "extensions": {
                        "mongot_extension": {
                            "base_url": "https://example.test/",
                            "name": "mongot-extension",
                            "version": "9.9.9",
                            "variants": {variant: checksum},
                        }
                    }
                },
                f,
            )

    def _make_tarball(self) -> bytes:
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w:gz") as tarball:
            for member_name, contents in (
                ("mongot-extension.so", b"fake-so-bytes"),
                ("mongot-extension.so.sig", b"fake-sig-bytes"),
            ):
                info = tarfile.TarInfo(member_name)
                info.size = len(contents)
                tarball.addfile(info, io.BytesIO(contents))
        return buf.getvalue()

    def test_returns_none_for_unknown_extension(self):
        self._write_conf("unused")
        self.assertIsNone(download_external_extension("add_fields_match", self.logger))

    def test_downloads_extracts_and_caches(self):
        tarball = self._make_tarball()
        self._write_conf(hashlib.sha256(tarball).hexdigest())

        response = unittest.mock.Mock(content=tarball)
        with patch(
            "buildscripts.resmokelib.extensions.download_external_extensions.requests.get",
            return_value=response,
        ) as get_mock:
            so_path = download_external_extension("mongot", self.logger)
            self.assertTrue(so_path.endswith("libmongot_extension.so"))
            with open(so_path, "rb") as f:
                self.assertEqual(f.read(), b"fake-so-bytes")
            self.assertTrue(os.path.exists(f"{so_path}.sig"))

            # A second call is served from the cache without re-downloading.
            self.assertEqual(download_external_extension("mongot", self.logger), so_path)
            self.assertEqual(get_mock.call_count, 1)

    def test_checksum_mismatch_raises(self):
        self._write_conf("0" * 64)
        response = unittest.mock.Mock(content=self._make_tarball())
        with patch(
            "buildscripts.resmokelib.extensions.download_external_extensions.requests.get",
            return_value=response,
        ):
            with self.assertRaisesRegex(RuntimeError, "Checksum mismatch"):
                download_external_extension("mongot", self.logger)


if __name__ == "__main__":
    unittest.main()
