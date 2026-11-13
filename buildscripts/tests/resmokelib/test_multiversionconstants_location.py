"""Unit tests to ensure buildscripts/resmokelib/multiversionconstants.py location for the one-click repro tool."""

import importlib
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from buildscripts.resmokelib import multiversionconstants
from buildscripts.resmokelib.multiversion import MultiversionConfigSubcommand
from buildscripts.resmokelib.multiversion.multiversion_service import (
    ASCLastPatchBehavior,
    DSCLastPatchBehavior,
    LastPatchSelectionError,
    MongoReleases,
    MongoVersion,
    MultiversionService,
)


class TestMultiversionconstantsLocation(unittest.TestCase):
    def setUp(self) -> None:
        self.enterContext(mock.patch.object(multiversionconstants, "_service", None))
        self.enterContext(mock.patch.object(multiversionconstants, "_service_manifest_file", None))

    def _reset_service(self) -> None:
        # subTest does not call setUp; each configuration needs its own service.
        multiversionconstants._service = None
        multiversionconstants._service_manifest_file = None

    def test_no_argument_factory_discovers_installed_manifests(self):
        version = MongoVersion(mongo_version="42.3.0")
        releases = MongoReleases(
            **{
                "featureCompatibilityVersions": ["41.0", "42.2", "42.3"],
                "longTermSupportReleases": ["41.0"],
                "eolVersions": [],
            }
        )
        selected = {
            "bin_suffix": "42.2",
            "discovery_request": {
                "request_type": "mongo_last_patch_version",
                "identifier": "42.2.0-rc9",
            },
        }
        other = {
            "bin_suffix": "41.0",
            "discovery_request": {"request_type": "mongo_release_version"},
        }
        layouts = {
            "root_fallback": {"root": [selected]},
            "directory_precedence": {
                "root": [
                    {
                        "bin_suffix": "42.3",
                        "discovery_request": {
                            "request_type": "mongo_last_patch_version",
                            "identifier": "42.3.0-rc1",
                        },
                    }
                ],
                "first": [selected],
            },
            "multiple_directories": {"first": [selected], "second": [other]},
            "missing": {},
            "empty": {"first": []},
            "duplicate_selection": {"first": [selected], "second": [selected]},
            "malformed": {"first": {"unexpected": "object"}},
        }
        for name, layout in layouts.items():
            with self.subTest(layout=name), tempfile.TemporaryDirectory() as directory:
                self._reset_service()
                root = Path(directory)
                first, second = root / "first", root / "second"
                first.mkdir()
                second.mkdir()
                directories = {"root": root, "first": first, "second": second}
                for location, entries in layout.items():
                    (directories[location] / "multiversion-downloads.json").write_text(
                        json.dumps(entries)
                    )
                with (
                    mock.patch.object(multiversionconstants._config, "RESMOKE_ROOT", str(root)),
                    mock.patch.object(
                        multiversionconstants._config,
                        "MULTIVERSION_DIRS",
                        [str(first), str(second)],
                    ),
                    mock.patch.object(MongoVersion, "from_yaml_file", return_value=version),
                    mock.patch.object(MongoReleases, "from_yaml_file", return_value=releases),
                    mock.patch.object(multiversionconstants, "get_expansion", return_value="atlas"),
                ):
                    service = multiversionconstants.get_multiversion_service()
                    if name in ("missing", "empty", "duplicate_selection", "malformed"):
                        with self.assertRaises(LastPatchSelectionError):
                            service.get_binary_name_for_version("last_patch", "mongod")
                    else:
                        self.assertEqual(
                            service.get_binary_name_for_version("last_patch", "mongod"),
                            "mongod-42.2",
                        )
                        self.assertEqual(service.get_last_patch_fcv(), "42.2")
                        self.assertIs(service, multiversionconstants.get_multiversion_service())

    def test_optional_config_does_not_read_required_dsc_metadata(self):
        version = MongoVersion(mongo_version="42.3.0")
        releases = MongoReleases(
            **{
                "featureCompatibilityVersions": ["41.0", "42.2", "42.3"],
                "longTermSupportReleases": ["41.0"],
                "eolVersions": [],
            }
        )
        for failure in (
            FileNotFoundError("manifest not downloaded yet"),
            ValueError("malformed manifest"),
        ):
            loader = mock.Mock(side_effect=failure)
            service = MultiversionService(
                version, releases, DSCLastPatchBehavior(version, releases, loader)
            )
            with (
                self.subTest(failure=type(failure).__name__),
                mock.patch.object(
                    multiversionconstants, "get_multiversion_service", return_value=service
                ),
                mock.patch(
                    "buildscripts.resmokelib.multiversion.multiversion_service.find_previous_release_tag",
                    return_value=None,
                ),
            ):
                config = MultiversionConfigSubcommand.determine_multiversion_config()
                self.assertEqual(config.last_lts_fcv, "41.0")
                self.assertEqual(config.last_continuous_fcv, "42.2")
                self.assertIsNone(config.last_patch_version)
                self.assertIsNone(config.last_patch_fcv)
                with tempfile.TemporaryDirectory() as directory, mock.patch("sys.stdout"):
                    output = Path(directory) / "multiversion-config.yml"
                    MultiversionConfigSubcommand({"config_file_output": str(output)}).execute()
                    text = output.read_text()
                    self.assertIn("last_lts_fcv:", text)
                    self.assertIn("last_continuous_fcv:", text)
                    self.assertNotIn("last_patch_version:", text)
                loader.assert_not_called()
                with self.assertRaises(LastPatchSelectionError):
                    MultiversionConfigSubcommand.determine_multiversion_config(
                        include_last_patch=True
                    )

    def test_import_does_not_construct_service_before_manifest_is_supplied(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            version_file = root / "version.yml"
            releases_file = root / "releases.yml"
            manifest_file = root / "chosen-dsc.json"
            version_file.write_text(json.dumps({"mongo_version": "42.3.0"}))
            releases_file.write_text(
                json.dumps(
                    {
                        "featureCompatibilityVersions": ["41.0", "42.2", "42.3"],
                        "longTermSupportReleases": ["41.0"],
                        "eolVersions": [],
                    }
                )
            )
            manifest_file.write_text(
                json.dumps(
                    [
                        {
                            "bin_suffix": "42.2",
                            "discovery_request": {
                                "request_type": "mongo_last_patch_version",
                                "identifier": "42.2.0-rc9",
                            },
                        }
                    ]
                )
            )
            script = f"""
from buildscripts.resmokelib import config, multiversionsetupconstants
config.MONGO_VERSION_FILE = {str(version_file)!r}
config.RELEASES_FILE = {str(releases_file)!r}
multiversionsetupconstants.USE_EXISTING_RELEASES_FILE = True
from buildscripts.resmokelib import multiversionconstants
assert multiversionconstants.LATEST_FCV == '42.3'
assert multiversionconstants._service is None, 'import constructed the service too early'
service = multiversionconstants.get_multiversion_service(manifest_file={str(manifest_file)!r})
assert service is multiversionconstants.get_multiversion_service()
assert service.get_last_patch_version() == '42.2.0-rc9'
assert service.get_last_patch_fcv() == '42.2'
assert service.get_binary_name_for_version('last_patch', 'mongod') == 'mongod-42.2'
"""
            env = os.environ.copy()
            env["DB_CONTRIB_TOOL_RELEASE_FEED_SOURCE"] = "atlas"
            env["PYTHONPATH"] = os.pathsep.join(
                str(Path(path or os.getcwd()).resolve()) for path in sys.path
            )
            result = subprocess.run(
                [sys.executable, "-c", script],
                cwd=directory,
                env=env,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_factory_constructs_one_service_and_selects_the_behavior_once(self):
        version = MongoVersion(mongo_version="42.3.0")
        releases = MongoReleases(
            **{
                "featureCompatibilityVersions": ["41.0", "42.2", "42.3"],
                "longTermSupportReleases": ["41.0"],
                "eolVersions": [],
            }
        )
        for source, expected in (("public", ASCLastPatchBehavior), ("atlas", DSCLastPatchBehavior)):
            for initial_manifest in (None, "selected.json"):
                with (
                    self.subTest(source=source, manifest=initial_manifest),
                    mock.patch.object(MongoVersion, "from_yaml_file", return_value=version),
                    mock.patch.object(MongoReleases, "from_yaml_file", return_value=releases),
                    mock.patch.object(multiversionconstants, "get_expansion", return_value=source),
                    mock.patch.dict("os.environ", {"DB_CONTRIB_TOOL_RELEASE_FEED_SOURCE": ""}),
                    mock.patch.object(multiversionconstants, "_read_downloads") as read,
                ):
                    self._reset_service()
                    service = (
                        multiversionconstants.get_multiversion_service()
                        if initial_manifest is None
                        else multiversionconstants.get_multiversion_service(
                            manifest_file=initial_manifest
                        )
                    )
                    self.assertIs(service, multiversionconstants.get_multiversion_service())
                    self.assertIs(
                        service,
                        multiversionconstants.get_multiversion_service(
                            manifest_file=initial_manifest,
                        ),
                    )
                    self.assertIsInstance(service.last_patch_behavior, expected)
                    with self.assertRaisesRegex(ValueError, "already initialized"):
                        multiversionconstants.get_multiversion_service(
                            manifest_file="different.json"
                        )
                    self.assertIs(service, multiversionconstants.get_multiversion_service())
                    read.assert_not_called()

    def test_multiversionconstants_location(self):
        multiversionconstants_module_name = "buildscripts.resmokelib.multiversionconstants"
        try:
            under_test_module = importlib.import_module(multiversionconstants_module_name)
        except ImportError:
            self.fail(
                f"Failed to import `{multiversionconstants_module_name}` module. One-click"
                f" repro tool (https://github.com/10gen/db-contrib-tools) requires this"
                f" module. If the module was changed, one-click repro tool should also"
                f" be updated. Please reach out in #server-testing slack channel."
            )
        else:
            expected_consts = ["LAST_LTS_FCV", "LAST_CONTINUOUS_FCV"]
            for const in expected_consts:
                self.assertTrue(
                    hasattr(under_test_module, const),
                    f"`{const}` constant is not found in `{multiversionconstants_module_name}`"
                    f" module. One-click repro tool (https://github.com/10gen/db-contrib-tools)"
                    f" uses this constant. If the module was changed, one-click repro tool"
                    f" should also be updated. Please reach out in #server-testing slack channel.",
                )


if __name__ == "__main__":
    unittest.main()
