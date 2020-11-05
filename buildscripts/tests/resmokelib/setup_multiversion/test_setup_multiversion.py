"""Unit tests for buildscripts/resmokelib/setup_multiversion/setup_multiversion.py."""
# pylint: disable=missing-docstring
import os
import unittest

from evergreen import RetryingEvergreenApi

from buildscripts.resmokelib.setup_multiversion.setup_multiversion import download_mongodb, get_evergreen_api


class TestGetEvgApi(unittest.TestCase):
    """Unit tests for the get_evergreen_api() function."""

    def test_incorrect_evergreen_config(self):
        evergreen_config = "some-random-file-i-hope-does-not-exist"
        self.assertRaises(Exception, get_evergreen_api, evergreen_config)

    def test_not_passing_evergreen_config(self):
        evergreen_config = None
        evg_api = get_evergreen_api(evergreen_config)
        self.assertIsInstance(evg_api, RetryingEvergreenApi)


class TestDownloadMongodb(unittest.TestCase):
    """Unit tests for the download_mongodb() function."""

    def test_download_mongodb(self):
        url = "https://mciuploads.s3.amazonaws.com/mongodb-mongo-v4.4/ubuntu1804" \
              "/f3699db3cf64566a0f87a84cdf9d2f26a6ebff73/binaries" \
              "/mongo-mongodb_mongo_v4.4_ubuntu1804_f3699db3cf64566a0f87a84cdf9d2f26a6ebff73_20_11_02_22_53_54.tgz"
        file = download_mongodb(url)
        self.assertTrue(os.path.exists(file))
        if os.path.exists(file):
            os.remove(file)
