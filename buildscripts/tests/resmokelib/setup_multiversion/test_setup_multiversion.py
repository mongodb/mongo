"""Unit tests for buildscripts/resmokelib/setup_multiversion/setup_multiversion.py."""
# pylint: disable=missing-docstring
import unittest
from argparse import Namespace

import requests
from mock import patch

from buildscripts.resmokelib.utils import evergreen_conn
from buildscripts.resmokelib.setup_multiversion.config import SetupMultiversionConfig
from buildscripts.resmokelib.setup_multiversion.setup_multiversion import SetupMultiversion, _DownloadOptions


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
                    resp.status_code = 500
                    raise requests.HTTPError(response=resp)
                raise StopIteration

        mock_versions_by_project.return_value = DummyIterator()

        urls = self.setup_multiversion.get_latest_urls(version)
        self.assertEqual(urls, {})

    @patch("evergreen.version.Version")
    @patch("evergreen.api.EvergreenApi.versions_by_project")
    @patch("buildscripts.resmokelib.utils.evergreen_conn.get_compile_artifact_urls")
    def test_no_compile_artifacts(self, mock_get_compile_artifact_urls, mock_versions_by_project,
                                  mock_version):
        mock_version.build_variants_map = {self.buildvariant_name: "build_id"}
        mock_versions_by_project.return_value = iter([mock_version])
        mock_get_compile_artifact_urls.return_value = {}

        urls = self.setup_multiversion.get_latest_urls("4.4")
        self.assertEqual(urls, {})

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

        urls = self.setup_multiversion.get_latest_urls("4.4")
        self.assertEqual(urls, expected_urls)

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

        urls = self.setup_multiversion.get_latest_urls("4.4")
        self.assertEqual(urls, expected_urls)

    @patch("evergreen.version.Version")
    @patch("evergreen.api.EvergreenApi.versions_by_project")
    @patch("buildscripts.resmokelib.utils.evergreen_conn.get_compile_artifact_urls")
    def test_fallback_to_generic_buildvariant(self, mock_get_compile_artifact_urls,
                                              mock_versions_by_project, mock_version):
        expected_urls = {
            "Binaries":
                "https://mciuploads.s3.amazonaws.com/mongodb-mongo-master/ubuntu1804/90f767adbb1901d007ee4dd8714f53402d893669/binaries/mongo-mongodb_mongo_master_ubuntu1804_90f767adbb1901d007ee4dd8714f53402d893669_20_11_30_03_14_30.tgz"
        }

        mock_version.build_variants_map = {self.generic_buildvariant_name: "build_id"}
        mock_versions_by_project.return_value = iter([mock_version])
        mock_get_compile_artifact_urls.return_value = expected_urls

        urls = self.setup_multiversion.get_latest_urls("4.4")
        self.assertEqual(urls, expected_urls)

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

        urls = self.setup_multiversion.get_latest_urls("master", start_from_revision)
        self.assertEqual(urls, expected_urls)


class TestSetupMultiversionGetUrls(TestSetupMultiversionBase):
    @patch("evergreen.version.Version")
    @patch("buildscripts.resmokelib.utils.evergreen_conn.get_evergreen_project_and_version")
    @patch("buildscripts.resmokelib.setup_multiversion.github_conn.get_git_tag_and_commit")
    @patch("buildscripts.resmokelib.utils.evergreen_conn.get_compile_artifact_urls")
    def test_urls_found(self, mock_get_compile_artifact_urls, mock_get_git_tag_and_commit,
                        mock_get_evergreen_project_and_version, mock_version):
        expected_urls = {
            "Binaries":
                "https://mciuploads.s3.amazonaws.com/mongodb-mongo-master/ubuntu1804/90f767adbb1901d007ee4dd8714f53402d893669/binaries/mongo-mongodb_mongo_master_ubuntu1804_90f767adbb1901d007ee4dd8714f53402d893669_20_11_30_03_14_30.tgz"
        }

        mock_get_git_tag_and_commit.return_value = ("git_tag", "commit_hash")
        mock_version.build_variants_map = {self.buildvariant_name: "build_id"}
        mock_get_evergreen_project_and_version.return_value = ("mongodb-mongo-v4.4", mock_version)
        mock_get_compile_artifact_urls.return_value = expected_urls

        urls = self.setup_multiversion.get_urls("4.4.1")
        self.assertEqual(urls, expected_urls)

    @patch("evergreen.version.Version")
    @patch("buildscripts.resmokelib.utils.evergreen_conn.get_evergreen_project_and_version")
    @patch("buildscripts.resmokelib.setup_multiversion.github_conn.get_git_tag_and_commit")
    @patch("buildscripts.resmokelib.utils.evergreen_conn.get_compile_artifact_urls")
    def test_urls_not_found(self, mock_get_compile_artifact_urls, mock_get_git_tag_and_commit,
                            mock_get_evergreen_project_and_version, mock_version):
        mock_get_git_tag_and_commit.return_value = ("git_tag", "commit_hash")
        mock_version.build_variants_map = {self.buildvariant_name: "build_id"}
        mock_get_evergreen_project_and_version.return_value = ("mongodb-mongo-v4.4", mock_version)
        mock_get_compile_artifact_urls.return_value = {}

        urls = self.setup_multiversion.get_urls("4.4.1")
        self.assertEqual(urls, {})

    @patch("evergreen.version.Version")
    @patch("buildscripts.resmokelib.utils.evergreen_conn.get_evergreen_project_and_version")
    @patch("buildscripts.resmokelib.setup_multiversion.github_conn.get_git_tag_and_commit")
    @patch("buildscripts.resmokelib.utils.evergreen_conn.get_compile_artifact_urls")
    def test_fallback_to_generic_buildvariant(self, mock_get_compile_artifact_urls,
                                              mock_get_git_tag_and_commit,
                                              mock_get_evergreen_project_and_version, mock_version):
        expected_urls = {
            "Binaries":
                "https://mciuploads.s3.amazonaws.com/mongodb-mongo-master/ubuntu1804/90f767adbb1901d007ee4dd8714f53402d893669/binaries/mongo-mongodb_mongo_master_ubuntu1804_90f767adbb1901d007ee4dd8714f53402d893669_20_11_30_03_14_30.tgz"
        }

        mock_get_git_tag_and_commit.return_value = ("git_tag", "commit_hash")
        mock_version.build_variants_map = {self.generic_buildvariant_name: "build_id"}
        mock_get_evergreen_project_and_version.return_value = ("mongodb-mongo-v4.4", mock_version)
        mock_get_compile_artifact_urls.return_value = expected_urls

        urls = self.setup_multiversion.get_urls("4.4.1")
        self.assertEqual(urls, expected_urls)
