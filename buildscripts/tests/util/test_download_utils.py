"""Unit tests for buildscripts/util/download_utils.py."""

import unittest
from unittest.mock import MagicMock, patch

import botocore
import botocore.exceptions

from buildscripts.util.download_utils import (
    S3AccessError,
    download_from_s3_with_boto,
    extract_s3_bucket_key,
    get_s3_client,
)


def _client_error(code):
    return botocore.exceptions.ClientError({"Error": {"Code": code, "Message": code}}, "HeadObject")


class TestExtractS3BucketKey(unittest.TestCase):
    def test_virtual_hosted_style(self):
        bucket, key = extract_s3_bucket_key(
            "https://mciuploads.s3.amazonaws.com/proj/bin/mongo.zst"
        )
        self.assertEqual(bucket, "mciuploads")
        self.assertEqual(key, "proj/bin/mongo.zst")

    def test_regional_host(self):
        bucket, key = extract_s3_bucket_key("https://mciuploads.s3.us-east-1.amazonaws.com/a/b.zst")
        self.assertEqual(bucket, "mciuploads")
        self.assertEqual(key, "a/b.zst")


class TestDownloadFromS3WithBoto(unittest.TestCase):
    URL = "https://mciuploads.s3.amazonaws.com/proj/binaries/mongo-88287.zst"

    @patch("buildscripts.util.download_utils.get_s3_client")
    def test_signed_success(self, get_client):
        client = MagicMock()
        get_client.return_value = client
        download_from_s3_with_boto(self.URL, "/tmp/out")
        client.download_file.assert_called_once()

    @patch("buildscripts.util.download_utils.get_s3_client")
    def test_public_object_falls_back_to_unsigned(self, get_client):
        signed = MagicMock()
        signed.download_file.side_effect = _client_error("403")
        unsigned = MagicMock()  # anonymous access succeeds for a public bucket
        get_client.side_effect = [signed, unsigned]

        download_from_s3_with_boto(self.URL, "/tmp/out")

        unsigned.download_file.assert_called_once()

    @patch("buildscripts.util.download_utils.get_s3_client")
    def test_private_object_raises_actionable_error(self, get_client):
        # Both signed and anonymous access are forbidden -> private object.
        signed = MagicMock()
        signed.download_file.side_effect = _client_error("403")
        unsigned = MagicMock()
        unsigned.download_file.side_effect = _client_error("403")
        get_client.side_effect = [signed, unsigned]

        with self.assertRaises(S3AccessError) as ctx:
            download_from_s3_with_boto(self.URL, "/tmp/out")

        msg = str(ctx.exception)
        self.assertIn("private", msg)
        self.assertIn("presigned URL", msg)

    @patch("buildscripts.util.download_utils.get_s3_client")
    def test_non_403_error_is_reraised(self, get_client):
        signed = MagicMock()
        signed.download_file.side_effect = _client_error("500")
        get_client.return_value = signed

        with self.assertRaises(botocore.exceptions.ClientError):
            download_from_s3_with_boto(self.URL, "/tmp/out")


class TestGetS3Client(unittest.TestCase):
    EVG_ENV = {"aws_key_new": "AKIA_EVG", "aws_secret": "evg-secret"}

    @patch("buildscripts.util.download_utils.boto3")
    def test_uses_evergreen_credentials_when_present(self, boto3_mod):
        with patch.dict("os.environ", self.EVG_ENV, clear=False):
            get_s3_client()
        _, kwargs = boto3_mod.client.call_args
        self.assertEqual(kwargs["aws_access_key_id"], "AKIA_EVG")
        self.assertEqual(kwargs["aws_secret_access_key"], "evg-secret")

    @patch("buildscripts.util.download_utils.boto3")
    def test_no_credentials_falls_back_to_default_chain(self, boto3_mod):
        env = {k: "" for k in self.EVG_ENV}  # unset both
        with patch.dict("os.environ", env, clear=False):
            get_s3_client()
        _, kwargs = boto3_mod.client.call_args
        self.assertNotIn("aws_access_key_id", kwargs)

    @patch("buildscripts.util.download_utils.boto3")
    def test_unsigned_client_is_not_given_credentials(self, boto3_mod):
        with patch.dict("os.environ", self.EVG_ENV, clear=False):
            get_s3_client(config=botocore.client.Config(signature_version=botocore.UNSIGNED))
        _, kwargs = boto3_mod.client.call_args
        self.assertNotIn("aws_access_key_id", kwargs)


if __name__ == "__main__":
    unittest.main()
