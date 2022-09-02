"""Unit tests for buildscripts/resmokelib/utils/evergreen_conn.py."""
import unittest

from mock import patch
from requests import HTTPError
from evergreen import RetryingEvergreenApi

from buildscripts.resmokelib.utils import evergreen_conn
from buildscripts.resmokelib.setup_multiversion.config import SetupMultiversionConfig


class TestGetEvgApi(unittest.TestCase):
    def test_incorrect_evergreen_config(self):
        evergreen_config = "some-random-file-i-hope-does-not-exist"
        self.assertRaises(Exception, evergreen_conn.get_evergreen_api, evergreen_config)

    def test_not_passing_evergreen_config(self):
        evergreen_config = None
        evg_api = evergreen_conn.get_evergreen_api(evergreen_config)
        self.assertIsInstance(evg_api, RetryingEvergreenApi)


class TestGetBuildvariantName(unittest.TestCase):
    def setUp(self):
        raw_yaml = {
            "evergreen_buildvariants": [
                {
                    "name": "macos-any",
                    "edition": "base",
                    "platform": "osx",
                    "architecture": "x86_64",
                },
                {
                    "name": "macos-4.0",
                    "edition": "base",
                    "platform": "osx",
                    "architecture": "x86_64",
                    "versions": ["4.0"],
                },
            ]
        }
        self.config = SetupMultiversionConfig(raw_yaml)

    def test_version_4_0(self):
        edition = "base"
        platform = "osx"
        architecture = "x86_64"
        major_minor_version = "4.0"

        buildvariant_name = evergreen_conn.get_buildvariant_name(
            config=self.config, edition=edition, platform=platform, architecture=architecture,
            major_minor_version=major_minor_version)
        self.assertEqual(buildvariant_name, "macos-4.0")

    def test_any_version(self):
        edition = "base"
        platform = "osx"
        architecture = "x86_64"
        major_minor_version = "any"

        buildvariant_name = evergreen_conn.get_buildvariant_name(
            config=self.config, edition=edition, platform=platform, architecture=architecture,
            major_minor_version=major_minor_version)
        self.assertEqual(buildvariant_name, "macos-any")

    def test_buildvariant_not_found(self):
        edition = "test"
        platform = "test"
        architecture = "test"
        major_minor_version = "any"

        buildvariant_name = evergreen_conn.get_buildvariant_name(
            config=self.config, edition=edition, platform=platform, architecture=architecture,
            major_minor_version=major_minor_version)
        self.assertEqual(buildvariant_name, "")


class TestGetGenericBuildvariantName(unittest.TestCase):
    def setUp(self):
        raw_yaml = {
            "evergreen_buildvariants": [{
                "name": "generic-buildvariant-name",
                "edition": evergreen_conn.GENERIC_EDITION,
                "platform": evergreen_conn.GENERIC_PLATFORM,
                "architecture": evergreen_conn.GENERIC_ARCHITECTURE,
                "versions": ["3.4", "3.6", "4.0"],
            }, ]
        }
        self.config = SetupMultiversionConfig(raw_yaml)

    def test_buildvariant_found(self):
        major_minor_version = "4.0"
        generic_buildvariant_name = evergreen_conn.get_generic_buildvariant_name(
            config=self.config, major_minor_version=major_minor_version)
        self.assertEqual(generic_buildvariant_name, "generic-buildvariant-name")

    def test_buildvarinat_not_found(self):
        major_minor_version = "4.2"
        self.assertRaises(evergreen_conn.EvergreenConnError,
                          evergreen_conn.get_generic_buildvariant_name, self.config,
                          major_minor_version)


class TestGetEvergreenProjectAndVersion(unittest.TestCase):
    @patch("evergreen.version.Version")
    @patch("evergreen.api.EvergreenApi")
    def test_version_by_commit_hash_found(self, mock_evg_api, mock_version):
        commit_hash = "2b0d538db8c0c9b9d7992d4489ba7171c721dfb7"
        expected_evergreen_version_id = "mongodb_mongo_master_" + commit_hash

        def version_by_id_side_effect(ref):
            if ref == expected_evergreen_version_id:
                mock_version.version_id = expected_evergreen_version_id
                return mock_version
            raise HTTPError()

        mock_evg_api.version_by_id.side_effect = version_by_id_side_effect
        evg_version = evergreen_conn.get_evergreen_version(mock_evg_api, commit_hash)
        self.assertEqual(mock_version, evg_version)
        self.assertEqual(mock_version.version_id, expected_evergreen_version_id)

    @patch("evergreen.version.Version")
    @patch("evergreen.api.EvergreenApi")
    def test_version_by_evergreen_version_id_found(self, mock_evg_api, mock_version):
        evergreen_version_id = "61419efc61837d4ce457132b"

        def version_by_id_side_effect(ref):
            if ref == evergreen_version_id:
                mock_version.version_id = evergreen_version_id
                return mock_version
            raise HTTPError()

        mock_evg_api.version_by_id.side_effect = version_by_id_side_effect
        evg_version = evergreen_conn.get_evergreen_version(mock_evg_api, evergreen_version_id)
        self.assertEqual(mock_version, evg_version)
        self.assertEqual(mock_version.version_id, evergreen_version_id)

    @patch("evergreen.api.EvergreenApi")
    def test_version_not_found(self, mock_evg_api):
        mock_evg_api.version_by_id.side_effect = HTTPError
        evg_version = evergreen_conn.get_evergreen_version(mock_evg_api, "dummy")
        self.assertIsNone(evg_version)


class TestGetCompileArtifactUrls(unittest.TestCase):
    @patch("evergreen.version.Version")
    @patch("evergreen.api.EvergreenApi")
    def test_buildvariant_not_found(self, mock_evg_api, mock_version):
        buildvariant_name = "test"
        mock_version.build_variants_map = {"not-test": "build_id"}
        self.assertRaises(evergreen_conn.EvergreenConnError,
                          evergreen_conn.get_compile_artifact_urls, mock_evg_api, mock_version,
                          buildvariant_name)

    @patch("evergreen.task.Artifact")
    @patch("evergreen.task.Task")
    @patch("evergreen.task.Task")
    @patch("evergreen.build.Build")
    @patch("evergreen.version.Version")
    @patch("evergreen.api.EvergreenApi")
    def test_urls_found(self, mock_evg_api, mock_version, mock_build, mock_compile_task,
                        mock_push_task, mock_artifact):

        mock_compile_task.project_identifier = "dummy project id"

        expected_urls = {
            "Binaries":
                "https://mciuploads.s3.amazonaws.com/mongodb-mongo-master/ubuntu1804/90f767adbb1901d007ee4dd8714f53402d893669/binaries/mongo-mongodb_mongo_master_ubuntu1804_90f767adbb1901d007ee4dd8714f53402d893669_20_11_30_03_14_30.tgz",
            "project_identifier":
                "dummy project id"
        }
        mock_evg_api.build_by_id.return_value = mock_build
        mock_artifact.name = "Binaries"
        mock_artifact.url = expected_urls["Binaries"]
        mock_compile_task.display_name = "compile"
        mock_compile_task.artifacts = [mock_artifact]
        mock_compile_task.status = "success"
        mock_push_task.display_name = "push"
        mock_push_task.status = "success"
        mock_build.get_tasks.return_value = [mock_compile_task, mock_push_task]

        urls = evergreen_conn.get_compile_artifact_urls(mock_evg_api, mock_version, "test")
        self.assertEqual(urls, expected_urls)

    @patch("evergreen.task.Task")
    @patch("evergreen.task.Task")
    @patch("evergreen.build.Build")
    @patch("evergreen.version.Version")
    @patch("evergreen.api.EvergreenApi")
    def test_push_task_failed(self, mock_evg_api, mock_version, mock_build, mock_compile_task,
                              mock_push_task):
        mock_evg_api.build_by_id.return_value = mock_build
        mock_compile_task.display_name = "compile"
        mock_compile_task.status = "success"
        mock_push_task.display_name = "push"
        mock_push_task.status = "failed"
        mock_build.get_tasks.return_value = [mock_compile_task, mock_push_task]

        urls = evergreen_conn.get_compile_artifact_urls(mock_evg_api, mock_version, "test")
        self.assertEqual(urls, {})

    @patch("evergreen.task.Task")
    @patch("evergreen.build.Build")
    @patch("evergreen.version.Version")
    @patch("evergreen.api.EvergreenApi")
    def test_no_push_task(self, mock_evg_api, mock_version, mock_build, mock_compile_task):
        mock_evg_api.build_by_id.return_value = mock_build
        mock_compile_task.display_name = "compile"
        mock_compile_task.status = "success"
        mock_build.get_tasks.return_value = [mock_compile_task]

        urls = evergreen_conn.get_compile_artifact_urls(mock_evg_api, mock_version, "test")
        self.assertEqual(urls, {})

    @patch("evergreen.build.Build")
    @patch("evergreen.version.Version")
    @patch("evergreen.api.EvergreenApi")
    def test_no_tasks(self, mock_evg_api, mock_version, mock_build):
        mock_evg_api.build_by_id.return_value = mock_build
        mock_build.get_tasks.return_value = []

        urls = evergreen_conn.get_compile_artifact_urls(mock_evg_api, mock_version, "test")
        self.assertEqual(urls, {})
