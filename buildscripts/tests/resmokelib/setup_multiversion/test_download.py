"""Unit tests for buildscripts/resmokelib/setup_multiversion/download.py."""
# pylint: disable=missing-docstring,no-self-use
import os
import tempfile
import unittest

from botocore.exceptions import ClientError
from mock import patch

from buildscripts.resmokelib.setup_multiversion.download import download_mongodb, DownloadError


class TestDownloadMongodb(unittest.TestCase):
    @patch("boto3.s3.transfer.S3Transfer.download_file")
    def test_correct_url_1(self, mock_download_file):
        url = "https://mciuploads.s3.amazonaws.com/mongodb-mongo-v4.4/ubuntu1804/f3699db3cf64566a0f87a84cdf9d2f26a6ebff73/binaries/mongo-mongodb_mongo_v4.4_ubuntu1804_f3699db3cf64566a0f87a84cdf9d2f26a6ebff73_20_11_02_22_53_54.tgz"
        bucket = "mciuploads"
        key = "mongodb-mongo-v4.4/ubuntu1804/f3699db3cf64566a0f87a84cdf9d2f26a6ebff73/binaries/mongo-mongodb_mongo_v4.4_ubuntu1804_f3699db3cf64566a0f87a84cdf9d2f26a6ebff73_20_11_02_22_53_54.tgz"
        filename = os.path.join(
            tempfile.gettempdir(),
            "mongo-mongodb_mongo_v4.4_ubuntu1804_f3699db3cf64566a0f87a84cdf9d2f26a6ebff73_20_11_02_22_53_54.tgz"
        )

        download_mongodb(url)
        mock_download_file.assert_called_with(bucket=bucket, key=key, filename=filename,
                                              extra_args=None, callback=None)

    @patch("boto3.s3.transfer.S3Transfer.download_file")
    def test_correct_url_2(self, mock_download_file):
        url = "https://s3.amazonaws.com/mciuploads/mongodb-mongo-v3.4/ubuntu1604/68fadc9173d8565ff687b4b769700b48d35ca5d5/binaries/mongo-mongodb_mongo_v3.4_ubuntu1604_68fadc9173d8565ff687b4b769700b48d35ca5d5_20_03_31_15_44_18.tgz"
        bucket = "mciuploads"
        key = "mongodb-mongo-v3.4/ubuntu1604/68fadc9173d8565ff687b4b769700b48d35ca5d5/binaries/mongo-mongodb_mongo_v3.4_ubuntu1604_68fadc9173d8565ff687b4b769700b48d35ca5d5_20_03_31_15_44_18.tgz"
        filename = os.path.join(
            tempfile.gettempdir(),
            "mongo-mongodb_mongo_v3.4_ubuntu1604_68fadc9173d8565ff687b4b769700b48d35ca5d5_20_03_31_15_44_18.tgz"
        )

        download_mongodb(url)
        mock_download_file.assert_called_with(bucket=bucket, key=key, filename=filename,
                                              extra_args=None, callback=None)

    def test_incorrect_url(self):
        url = "https://mciuploads.s3.amazonaws.com/no-file-here"
        self.assertRaises(ClientError, download_mongodb, url)

    def test_no_url(self):
        url = ""
        self.assertRaises(DownloadError, download_mongodb, url)
