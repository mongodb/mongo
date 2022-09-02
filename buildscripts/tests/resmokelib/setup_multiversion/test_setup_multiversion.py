"""Unit tests for buildscripts/resmokelib/setup_multiversion/setup_multiversion.py."""
import unittest
from argparse import Namespace

import requests
from mock import patch

from buildscripts.resmokelib.utils import evergreen_conn
from buildscripts.resmokelib.setup_multiversion.config import SetupMultiversionConfig
from buildscripts.resmokelib.setup_multiversion.setup_multiversion import SetupMultiversion, _DownloadOptions, \
    infer_platform


class TestInferPlatform(unittest.TestCase):
    @patch("platform.system")
    def test_infer_platform_darwin(self, mock_system):
        mock_system.return_value = 'Darwin'
        pltf = infer_platform('base', "4.2")
        self.assertEqual(pltf, 'osx')
        pltf = infer_platform('enterprise', "4.2")
        self.assertEqual(pltf, 'osx')
        pltf = infer_platform('base', "4.0")
        self.assertEqual(pltf, 'osx')
        pltf = infer_platform(None, "4.2")
        self.assertEqual(pltf, 'osx')
        pltf = infer_platform('base', None)
        self.assertEqual(pltf, 'osx')
        pltf = infer_platform(None, None)
        self.assertEqual(pltf, 'osx')

    @patch("platform.system")
    def test_infer_platform_windows(self, mock_system):
        mock_system.return_value = 'Windows'
        pltf = infer_platform('base', "4.2")
        self.assertEqual(pltf, 'windows_x86_64-2012plus')
        pltf = infer_platform('enterprise', "4.2")
        self.assertEqual(pltf, 'windows')
        pltf = infer_platform('base', "4.0")
        self.assertEqual(pltf, 'windows')
        pltf = infer_platform(None, "4.2")
        self.assertEqual(pltf, 'windows')
        pltf = infer_platform('base', None)
        self.assertEqual(pltf, 'windows')
        pltf = infer_platform(None, None)
        self.assertEqual(pltf, 'windows')

    @patch("distro.minor_version")
    @patch("distro.major_version")
    @patch("distro.id")
    @patch("platform.system")
    def test_infer_platform_linux(self, mock_system, mock_id, mock_major, mock_minor):
        mock_system.return_value = 'Linux'
        mock_id.return_value = 'ubuntu'
        mock_major.return_value = '18'
        mock_minor.return_value = '04'
        pltf = infer_platform('base', "4.2")
        self.assertEqual(pltf, 'ubuntu1804')
        pltf = infer_platform('enterprise', "4.2")
        self.assertEqual(pltf, 'ubuntu1804')
        pltf = infer_platform('base', "4.0")
        self.assertEqual(pltf, 'ubuntu1804')
        pltf = infer_platform(None, "4.2")
        self.assertEqual(pltf, 'ubuntu1804')
        pltf = infer_platform('base', None)
        self.assertEqual(pltf, 'ubuntu1804')
        pltf = infer_platform(None, None)
        self.assertEqual(pltf, 'ubuntu1804')

        mock_id.return_value = 'rhel'
        mock_major.return_value = '8'
        mock_minor.return_value = '0'
        pltf = infer_platform('base', "4.2")
        self.assertEqual(pltf, 'rhel80')
        pltf = infer_platform('enterprise', "4.2")
        self.assertEqual(pltf, 'rhel80')
        pltf = infer_platform('base', "4.0")
        self.assertEqual(pltf, 'rhel80')
        pltf = infer_platform(None, "4.2")
        self.assertEqual(pltf, 'rhel80')
        pltf = infer_platform('base', None)
        self.assertEqual(pltf, 'rhel80')
        pltf = infer_platform(None, None)
        self.assertEqual(pltf, 'rhel80')

    @patch("distro.id")
    @patch("platform.system")
    def test_infer_platform_others(self, mock_system, mock_id):
        mock_system.return_value = 'Java'
        self.assertRaises(ValueError, infer_platform, 'enterprise', "4.2")
        self.assertRaises(ValueError, infer_platform, 'base', None)
        self.assertRaises(ValueError, infer_platform, None, "4.2")
        self.assertRaises(ValueError, infer_platform, None, None)
        mock_system.return_value = 'Linux'
        mock_id.return_value = 'debian'
        self.assertRaises(ValueError, infer_platform, 'enterprise', "4.2")
        self.assertRaises(ValueError, infer_platform, 'base', None)
        self.assertRaises(ValueError, infer_platform, None, "4.2")
        self.assertRaises(ValueError, infer_platform, None, None)


class TestSetupMultiversionBase(unittest.TestCase):
    def setUp(self):
        self.buildvariant_name = "buildvariant-name"
        self.generic_buildvariant_name = "generic-buildvariant-name"
        edition = "edition"
        platform = "platform"
        architecture = "architecture"
        raw_yaml_config = {
            "evergreen_projects": [
                "mongodb-mongo-master",
                "mongodb-mongo-v4.4",
            ], "evergreen_buildvariants": [
                {
                    "name": self.buildvariant_name,
                    "edition": edition,
                    "platform": platform,
                    "architecture": architecture,
                },
                {
                    "name": self.generic_buildvariant_name,
                    "edition": evergreen_conn.GENERIC_EDITION,
                    "platform": evergreen_conn.GENERIC_PLATFORM,
                    "architecture": evergreen_conn.GENERIC_ARCHITECTURE,
                },
            ]
        }

        download_options = _DownloadOptions(db=True, ds=False, da=False, dv=False)

        options = Namespace(
            install_dir="install",
            link_dir="link",
            edition=edition,
            mv_platform=platform,
            architecture=architecture,
            use_latest=False,
            versions=["4.2.1"],
            evergreen_config=None,
            github_oauth_token=None,
            download_options=download_options,
            debug=False,
        )
        with patch("buildscripts.resmokelib.setup_multiversion.config.SetupMultiversionConfig"
                   ) as mock_config:
            mock_config.return_value = SetupMultiversionConfig(raw_yaml_config)
            self.setup_multiversion = SetupMultiversion(**vars(options))


class TestSetupMultiversionGetLatestUrls(TestSetupMultiversionBase):
    @patch("evergreen.api.EvergreenApi.versions_by_project")
    def test_no_such_project(self, mock_versions_by_project):
        """Project `mongodb-mongo-v4.2.1` doesn't exist."""
        version = "4.2.1"

        class DummyIterator:
            def __init__(self):
                self.current = 0

            def __iter__(self):
                return self

            def __next__(self):
                if self.current == 0:
                    self.current += 1
                    resp = requests.models.Response()
                    resp.status_code = 404
                    raise requests.HTTPError(response=resp)
                raise StopIteration

        mock_versions_by_project.return_value = DummyIterator()

        urlinfo = self.setup_multiversion.get_latest_urls(version)
        self.assertEqual(urlinfo.urls, {})

    @patch("evergreen.version.Version")
    @patch("evergreen.api.EvergreenApi.versions_by_project")
    @patch("buildscripts.resmokelib.utils.evergreen_conn.get_compile_artifact_urls")
    def test_no_compile_artifacts(self, mock_get_compile_artifact_urls, mock_versions_by_project,
                                  mock_version):
        mock_version.build_variants_map = {self.buildvariant_name: "build_id"}
        mock_versions_by_project.return_value = iter([mock_version])
        mock_get_compile_artifact_urls.return_value = {}

        urlinfo = self.setup_multiversion.get_latest_urls("4.4")
        self.assertEqual(urlinfo.urls, {})

    @patch("evergreen.version.Version")
    @patch("evergreen.api.EvergreenApi.versions_by_project")
    @patch("buildscripts.resmokelib.utils.evergreen_conn.get_compile_artifact_urls")
    def test_urls_found_on_last_version(self, mock_get_compile_artifact_urls,
                                        mock_versions_by_project, mock_version):
        expected_urls = {
            "Binaries":
                "https://mciuploads.s3.amazonaws.com/mongodb-mongo-master/ubuntu1804/90f767adbb1901d007ee4dd8714f53402d893669/binaries/mongo-mongodb_mongo_master_ubuntu1804_90f767adbb1901d007ee4dd8714f53402d893669_20_11_30_03_14_30.tgz"
        }

        mock_version.build_variants_map = {self.buildvariant_name: "build_id"}
        mock_versions_by_project.return_value = iter([mock_version])
        mock_get_compile_artifact_urls.return_value = expected_urls

        urlinfo = self.setup_multiversion.get_latest_urls("4.4")
        self.assertEqual(urlinfo.urls, expected_urls)

    @patch("evergreen.version.Version")
    @patch("evergreen.version.Version")
    @patch("evergreen.api.EvergreenApi.versions_by_project")
    @patch("buildscripts.resmokelib.utils.evergreen_conn.get_compile_artifact_urls")
    def test_urls_found_on_not_last_version(self, mock_get_compile_artifact_urls,
                                            mock_versions_by_project, mock_version,
                                            mock_expected_version):
        expected_urls = {
            "Binaries":
                "https://mciuploads.s3.amazonaws.com/mongodb-mongo-master/ubuntu1804/90f767adbb1901d007ee4dd8714f53402d893669/binaries/mongo-mongodb_mongo_master_ubuntu1804_90f767adbb1901d007ee4dd8714f53402d893669_20_11_30_03_14_30.tgz"
        }

        mock_version.build_variants_map = {self.buildvariant_name: "build_id"}
        mock_expected_version.build_variants_map = {self.buildvariant_name: "build_id"}
        evg_versions = [mock_version for _ in range(3)]
        evg_versions.append(mock_expected_version)
        mock_versions_by_project.return_value = iter(evg_versions)
        mock_get_compile_artifact_urls.side_effect = lambda evg_api, evg_version, buildvariant_name, ignore_failed_push: {
            (self.setup_multiversion.evg_api, mock_version, self.buildvariant_name, False): {},
            (self.setup_multiversion.evg_api, mock_expected_version, self.buildvariant_name, False):
                expected_urls,
        }[evg_api, evg_version, buildvariant_name, ignore_failed_push]

        urlinfo = self.setup_multiversion.get_latest_urls("4.4")
        self.assertEqual(urlinfo.urls, expected_urls)

    @patch("evergreen.version.Version")
    @patch("evergreen.version.Version")
    @patch("evergreen.api.EvergreenApi.versions_by_project")
    @patch("buildscripts.resmokelib.utils.evergreen_conn.get_compile_artifact_urls")
    def test_start_from_revision(self, mock_get_compile_artifact_urls, mock_versions_by_project,
                                 mock_version, mock_expected_version):
        start_from_revision = "90f767adbb1901d007ee4dd8714f53402d893669"
        unexpected_urls = {
            "Binaries":
                "https://mciuploads.s3.amazonaws.com/project/build_variant/revision/binaries/unexpected.tgz"
        }
        expected_urls = {
            "Binaries":
                "https://mciuploads.s3.amazonaws.com/project/build_variant/90f767adbb1901d007ee4dd8714f53402d893669/binaries/expected.tgz"
        }

        mock_version.build_variants_map = {self.buildvariant_name: "build_id"}
        mock_expected_version.build_variants_map = {self.buildvariant_name: "build_id"}
        mock_expected_version.revision = start_from_revision

        evg_versions = [mock_version for _ in range(3)]
        evg_versions.append(mock_expected_version)
        mock_versions_by_project.return_value = iter(evg_versions)

        mock_get_compile_artifact_urls.side_effect = lambda evg_api, evg_version, buildvariant_name, ignore_failed_push: {
            (self.setup_multiversion.evg_api, mock_version, self.buildvariant_name, False):
                unexpected_urls,
            (self.setup_multiversion.evg_api, mock_expected_version, self.buildvariant_name, False):
                expected_urls,
        }[evg_api, evg_version, buildvariant_name, ignore_failed_push]

        urlinfo = self.setup_multiversion.get_latest_urls("master", start_from_revision)
        self.assertEqual(urlinfo.urls, expected_urls)


class TestSetupMultiversionGetUrls(TestSetupMultiversionBase):
    @patch("evergreen.version.Version")
    @patch("buildscripts.resmokelib.utils.evergreen_conn.get_evergreen_version")
    @patch("buildscripts.resmokelib.setup_multiversion.github_conn.get_git_tag_and_commit")
    @patch("buildscripts.resmokelib.utils.evergreen_conn.get_compile_artifact_urls")
    def test_urls_by_binary_version_found(self, mock_get_compile_artifact_urls,
                                          mock_get_git_tag_and_commit, mock_get_evergreen_version,
                                          mock_version):
        expected_urls = {
            "Binaries":
                "https://mciuploads.s3.amazonaws.com/mongodb-mongo-master/ubuntu1804/90f767adbb1901d007ee4dd8714f53402d893669/binaries/mongo-mongodb_mongo_master_ubuntu1804_90f767adbb1901d007ee4dd8714f53402d893669_20_11_30_03_14_30.tgz"
        }

        mock_get_git_tag_and_commit.return_value = ("r4.4.1",
                                                    "90f767adbb1901d007ee4dd8714f53402d893669")
        mock_version.build_variants_map = {self.buildvariant_name: "build_id"}
        mock_version.project_identifier = "mongodb-mongo-v4.4"
        mock_get_evergreen_version.return_value = mock_version
        mock_get_compile_artifact_urls.return_value = expected_urls

        urlinfo = self.setup_multiversion.get_urls("4.4.1")
        self.assertEqual(urlinfo.urls, expected_urls)

    @patch("evergreen.version.Version")
    @patch("buildscripts.resmokelib.utils.evergreen_conn.get_evergreen_version")
    @patch("buildscripts.resmokelib.utils.evergreen_conn.get_compile_artifact_urls")
    def test_urls_by_commit_hash_found(self, mock_get_compile_artifact_urls,
                                       mock_get_evergreen_version, mock_version):
        expected_urls = {
            "Binaries":
                "https://mciuploads.s3.amazonaws.com/mongodb-mongo-master/ubuntu1804/90f767adbb1901d007ee4dd8714f53402d893669/binaries/mongo-mongodb_mongo_master_ubuntu1804_90f767adbb1901d007ee4dd8714f53402d893669_20_11_30_03_14_30.tgz"
        }

        mock_version.build_variants_map = {self.buildvariant_name: "build_id"}
        mock_version.project_identifier = "mongodb-mongo-v4.4"
        mock_get_evergreen_version.return_value = mock_version
        mock_get_compile_artifact_urls.return_value = expected_urls

        urlinfo = self.setup_multiversion.get_urls("90f767adbb1901d007ee4dd8714f53402d893669")
        self.assertEqual(urlinfo.urls, expected_urls)

    @patch("evergreen.version.Version")
    @patch("buildscripts.resmokelib.utils.evergreen_conn.get_evergreen_version")
    @patch("buildscripts.resmokelib.setup_multiversion.github_conn.get_git_tag_and_commit")
    @patch("buildscripts.resmokelib.utils.evergreen_conn.get_compile_artifact_urls")
    def test_urls_not_found(self, mock_get_compile_artifact_urls, mock_get_git_tag_and_commit,
                            mock_get_evergreen_version, mock_version):
        mock_get_git_tag_and_commit.return_value = ("r4.4.1",
                                                    "90f767adbb1901d007ee4dd8714f53402d893669")
        mock_version.version_id = "dummy-version-id"
        mock_version.build_variants_map = {self.buildvariant_name: "build_id"}
        mock_version.project_identifier = "mongodb-mongo-v4.4"
        mock_get_evergreen_version.return_value = mock_version
        mock_get_compile_artifact_urls.return_value = {}

        urlinfo = self.setup_multiversion.get_urls("4.4.1")
        self.assertEqual(urlinfo.urls, {})
        self.assertEqual(urlinfo.evg_version_id, mock_version.version_id)

    @patch("buildscripts.resmokelib.utils.evergreen_conn.get_evergreen_version")
    @patch("buildscripts.resmokelib.setup_multiversion.github_conn.get_git_tag_and_commit")
    def test_evg_version_not_found(self, mock_get_git_tag_and_commit, mock_get_evergreen_version):
        mock_get_git_tag_and_commit.return_value = ("r4.4.1",
                                                    "90f767adbb1901d007ee4dd8714f53402d893669")
        mock_get_evergreen_version.return_value = None

        urlinfo = self.setup_multiversion.get_urls("4.4.1")
        self.assertEqual(urlinfo.urls, {})
        self.assertEqual(urlinfo.evg_version_id, None)
