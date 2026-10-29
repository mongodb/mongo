"""Tests for task-owned, conditional S3 artifact uploads."""

import json
import tempfile
import unittest
from pathlib import Path
from typing import Any
from unittest.mock import Mock, patch

from botocore.exceptions import ClientError
from botocore.session import Session
from botocore.stub import ANY, Stubber

from buildscripts.task_artifacts import TaskIdentity, upload_artifact


class TaskArtifactsTest(unittest.TestCase):
    def setUp(self):
        self.identity = TaskIdentity("project", "variant", "version", "archive", "task-id", "0")
        self.client = Mock()
        self.client.head_object.side_effect = ClientError({"Error": {"Code": "404"}}, "HeadObject")

    def existing_object(self, execution: str = "0") -> dict[str, Any]:
        return {
            "ETag": '"existing-etag"',
            "Metadata": {
                "build-variant": self.identity.build_variant,
                "task-name": self.identity.task_name,
                "task-id": self.identity.task_id,
                "execution": execution,
            },
        }

    def test_other_variants_and_tasks_have_disjoint_prefixes(self):
        identities = [
            self.identity,
            TaskIdentity("project", "other-variant", "version", "archive", "task-id-2", "0"),
            TaskIdentity("project", "variant", "version", "package", "task-id-3", "0"),
        ]
        keys = {identity.key("binaries/mongo.tgz") for identity in identities}
        self.assertEqual(3, len(keys))
        self.assertEqual(
            "project/variant/version/task-artifacts/archive/binaries/mongo.tgz",
            self.identity.key("binaries/mongo.tgz"),
        )

    def test_invalid_names_cannot_escape_the_task_prefix(self):
        for name in ["", "/file", "../file", "binaries/../file", "./file", "a//b", "a\\b", "a\nb"]:
            with self.subTest(name=name), self.assertRaises(ValueError):
                self.identity.key(name)
        with self.assertRaises(ValueError):
            TaskIdentity("project", "../other", "version", "archive", "id", "0")
        with self.assertRaises(ValueError):
            TaskIdentity("project", "variant", "version", "archive", "id", "unknown")

    def test_small_upload_is_conditional_and_records_its_owner(self):
        with tempfile.TemporaryDirectory() as directory:
            local = Path(directory) / "mongo.tgz"
            local.write_bytes(b"binary")
            manifest = Path(directory) / "artifact.json"
            upload_artifact(
                self.client,
                self.identity,
                local,
                "binaries/mongo.tgz",
                manifest,
                display_name="Binaries",
                content_type="application/gzip",
            )
            kwargs = self.client.put_object.call_args.kwargs
            self.assertEqual("*", kwargs["IfNoneMatch"])
            self.assertEqual(self.identity.key("binaries/mongo.tgz"), kwargs["Key"])
            self.assertEqual("archive", kwargs["Metadata"]["task-name"])
            self.assertEqual("task-id", kwargs["Metadata"]["task-id"])
            artifact = json.loads(manifest.read_text())[0]
            self.assertEqual("Binaries", artifact["name"])
            self.assertEqual("signed", artifact["visibility"])
            self.assertEqual(kwargs["Key"], artifact["filekey"])
            self.assertEqual("aws_key_new", artifact["aws_key_var_name"])
            self.assertNotIn("aws_secret", artifact)

    def test_existing_object_and_racing_writer_fail_without_attaching(self):
        for code in ["PreconditionFailed", "ConditionalRequestConflict", "AccessDenied"]:
            with self.subTest(code=code), tempfile.TemporaryDirectory() as directory:
                local = Path(directory) / "file"
                local.write_bytes(b"binary")
                manifest = Path(directory) / "artifact.json"
                manifest.write_text('[{"name": "stale"}]')
                self.client.put_object.side_effect = ClientError(
                    {"Error": {"Code": code}}, "PutObject"
                )
                with self.assertRaises(ClientError):
                    upload_artifact(self.client, self.identity, local, "file", manifest)
                self.assertEqual([], json.loads(manifest.read_text()))
                self.assertEqual("*", self.client.put_object.call_args.kwargs["IfNoneMatch"])

    def test_restart_replaces_changed_artifact_at_the_same_key(self):
        restarted = TaskIdentity("project", "variant", "version", "archive", "task-id", "1")
        self.client.head_object.side_effect = [
            ClientError({"Error": {"Code": "404"}}, "HeadObject"),
            self.existing_object(),
        ]
        uploaded = []

        def record_upload(**kwargs):
            uploaded.append({**kwargs, "Body": kwargs["Body"].read()})

        self.client.put_object.side_effect = record_upload
        with tempfile.TemporaryDirectory() as directory:
            local = Path(directory) / "file"
            manifest = Path(directory) / "artifact.json"
            local.write_bytes(b"first build")
            upload_artifact(self.client, self.identity, local, "file", manifest)
            local.write_bytes(b"rebuilt artifact with different bytes")
            upload_artifact(self.client, restarted, local, "file", manifest)
            artifact = json.loads(manifest.read_text())[0]
            self.assertEqual(local.stat().st_size, artifact["file_size"])
            self.assertEqual(uploaded[0]["Key"], artifact["filekey"])
        self.assertEqual(uploaded[0]["Key"], uploaded[1]["Key"])
        self.assertEqual("*", uploaded[0]["IfNoneMatch"])
        self.assertEqual('"existing-etag"', uploaded[1]["IfMatch"])
        self.assertNotIn("IfNoneMatch", uploaded[1])
        self.assertEqual(b"rebuilt artifact with different bytes", uploaded[1]["Body"])
        self.assertEqual("1", uploaded[1]["Metadata"]["execution"])

    def test_same_execution_can_retry_a_successful_upload(self):
        self.client.head_object.side_effect = None
        self.client.head_object.return_value = self.existing_object()
        with tempfile.TemporaryDirectory() as directory:
            local = Path(directory) / "file"
            local.write_bytes(b"binary")
            manifest = Path(directory) / "artifact.json"
            upload_artifact(self.client, self.identity, local, "file", manifest)
            self.assertEqual(1, len(json.loads(manifest.read_text())))
        self.assertEqual('"existing-etag"', self.client.put_object.call_args.kwargs["IfMatch"])

    def test_unowned_objects_and_newer_executions_cannot_be_replaced(self):
        for field, value in [
            ("task-id", "other-task"),
            ("task-id", None),
            ("task-name", "other-name"),
            ("build-variant", "other-variant"),
            ("execution", "1"),
            ("execution", "unknown"),
            ("execution", None),
        ]:
            with self.subTest(field=field, value=value), tempfile.TemporaryDirectory() as directory:
                existing = self.existing_object()
                if value is None:
                    del existing["Metadata"][field]
                else:
                    existing["Metadata"][field] = value
                self.client.head_object.side_effect = None
                self.client.head_object.return_value = existing
                local = Path(directory) / "file"
                local.write_bytes(b"binary")
                manifest = Path(directory) / "artifact.json"
                manifest.write_text('[{"name": "stale"}]')
                with self.assertRaises(ValueError):
                    upload_artifact(self.client, self.identity, local, "file", manifest)
                self.client.put_object.assert_not_called()
                self.client.create_multipart_upload.assert_not_called()
                self.assertEqual([], json.loads(manifest.read_text()))

    def test_existing_object_without_an_etag_cannot_be_replaced(self):
        self.client.head_object.side_effect = None
        existing = self.existing_object()
        del existing["ETag"]
        self.client.head_object.return_value = existing
        with tempfile.TemporaryDirectory() as directory:
            local = Path(directory) / "file"
            local.write_bytes(b"binary")
            manifest = Path(directory) / "artifact.json"
            with self.assertRaises(ValueError):
                upload_artifact(self.client, self.identity, local, "file", manifest)
            self.assertEqual([], json.loads(manifest.read_text()))
        self.client.put_object.assert_not_called()

    def test_missing_object_can_be_created_without_bucket_list_permission(self):
        for code in ["403", "AccessDenied"]:
            with self.subTest(code=code), tempfile.TemporaryDirectory() as directory:
                self.client.head_object.side_effect = ClientError(
                    {"Error": {"Code": code}}, "HeadObject"
                )
                local = Path(directory) / "file"
                local.write_bytes(b"binary")
                manifest = Path(directory) / "artifact.json"
                upload_artifact(self.client, self.identity, local, "file", manifest)
                self.assertEqual("*", self.client.put_object.call_args.kwargs["IfNoneMatch"])
                self.assertNotIn("IfMatch", self.client.put_object.call_args.kwargs)
                self.assertEqual(1, len(json.loads(manifest.read_text())))

    def test_unreadable_existing_object_cannot_be_overwritten(self):
        self.client.head_object.side_effect = ClientError({"Error": {"Code": "403"}}, "HeadObject")
        self.client.put_object.side_effect = ClientError(
            {"Error": {"Code": "PreconditionFailed"}}, "PutObject"
        )
        with tempfile.TemporaryDirectory() as directory:
            local = Path(directory) / "file"
            local.write_bytes(b"binary")
            manifest = Path(directory) / "artifact.json"
            manifest.write_text('[{"name": "stale"}]')
            with self.assertRaises(ClientError):
                upload_artifact(self.client, self.identity, local, "file", manifest)
            self.assertEqual([], json.loads(manifest.read_text()))
        self.client.put_object.assert_called_once()
        self.assertEqual("*", self.client.put_object.call_args.kwargs["IfNoneMatch"])

    def test_other_head_errors_do_not_attempt_a_write(self):
        self.client.head_object.side_effect = ClientError(
            {"Error": {"Code": "InternalError"}}, "HeadObject"
        )
        with tempfile.TemporaryDirectory() as directory:
            local = Path(directory) / "file"
            local.write_bytes(b"binary")
            with self.assertRaises(ClientError):
                upload_artifact(self.client, self.identity, local, "file", Path(directory) / "out")
        self.client.put_object.assert_not_called()

    def test_multipart_creation_after_ambiguous_head_remains_conditional(self):
        self.client.head_object.side_effect = ClientError({"Error": {"Code": "403"}}, "HeadObject")
        self.client.create_multipart_upload.return_value = {"UploadId": "upload"}
        self.client.upload_part.return_value = {"ETag": "part"}
        with tempfile.TemporaryDirectory() as directory:
            local = Path(directory) / "file"
            local.write_bytes(b"abcdefgh")
            with patch("buildscripts.task_artifacts.PART_SIZE", 4):
                upload_artifact(
                    self.client, self.identity, local, "file", Path(directory) / "artifact.json"
                )
        self.assertEqual("*", self.client.complete_multipart_upload.call_args.kwargs["IfNoneMatch"])

    def test_restart_fails_if_the_object_changes_after_the_ownership_check(self):
        self.client.head_object.side_effect = None
        self.client.head_object.return_value = self.existing_object()
        self.client.put_object.side_effect = ClientError(
            {"Error": {"Code": "PreconditionFailed"}}, "PutObject"
        )
        with tempfile.TemporaryDirectory() as directory:
            local = Path(directory) / "file"
            local.write_bytes(b"binary")
            manifest = Path(directory) / "artifact.json"
            with self.assertRaises(ClientError):
                upload_artifact(self.client, self.identity, local, "file", manifest)
            self.assertEqual([], json.loads(manifest.read_text()))
        self.assertEqual('"existing-etag"', self.client.put_object.call_args.kwargs["IfMatch"])

    def test_optional_missing_file_only_skips_missing_files(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest = Path(directory) / "artifact.json"
            missing = Path(directory) / "missing"
            upload_artifact(self.client, self.identity, missing, "file", manifest, optional=True)
            self.assertEqual([], json.loads(manifest.read_text()))
            self.client.put_object.assert_not_called()
            with self.assertRaises(FileNotFoundError):
                upload_artifact(self.client, self.identity, missing, "file", manifest)

    def test_visibility_requires_matching_permissions_before_accessing_s3(self):
        for permissions, visibility, message in [
            ("private", "public", "Public artifacts must use public-read permissions"),
            ("public-read", "signed", "Signed artifacts must use private permissions"),
        ]:
            with (
                self.subTest(permissions=permissions, visibility=visibility),
                tempfile.TemporaryDirectory() as directory,
            ):
                local = Path(directory) / "file"
                local.write_bytes(b"artifact")
                manifest = Path(directory) / "artifact.json"
                manifest.write_text('[{"name": "stale"}]')
                with self.assertRaisesRegex(ValueError, message):
                    upload_artifact(
                        self.client,
                        self.identity,
                        local,
                        "file",
                        manifest,
                        permissions=permissions,
                        visibility=visibility,
                    )
                self.assertEqual([], json.loads(manifest.read_text()))
        self.client.head_object.assert_not_called()
        self.client.put_object.assert_not_called()
        self.client.create_multipart_upload.assert_not_called()

    def test_uploads_preserve_intelligent_tiering(self):
        for part_size in [4, 64]:
            with (
                self.subTest(part_size=part_size),
                tempfile.TemporaryDirectory() as directory,
                patch("buildscripts.task_artifacts.PART_SIZE", part_size),
            ):
                self.client.reset_mock()
                self.client.create_multipart_upload.return_value = {"UploadId": "upload"}
                self.client.upload_part.return_value = {"ETag": "part"}
                local = Path(directory) / "file"
                local.write_bytes(b"artifact bytes")
                upload_artifact(
                    self.client, self.identity, local, "file", Path(directory) / "artifact.json"
                )
                request = (
                    self.client.create_multipart_upload
                    if part_size == 4
                    else self.client.put_object
                )
                self.assertEqual(
                    "INTELLIGENT_TIERING", request.call_args.kwargs.get("StorageClass")
                )

    def test_active_and_unknown_content_types_fail_before_accessing_s3(self):
        for content_type in [
            "text/html",
            "application/xhtml+xml",
            "image/svg+xml",
            "application/javascript",
            "text/html; charset=utf-8",
            "application/octet-stream\r\nContent-Type: text/html",
            "unknown/type",
            "",
        ]:
            with (
                self.subTest(content_type=content_type),
                tempfile.TemporaryDirectory() as directory,
            ):
                manifest = Path(directory) / "artifact.json"
                manifest.write_text('[{"name": "stale"}]')
                with self.assertRaisesRegex(ValueError, "Unsupported artifact content type"):
                    upload_artifact(
                        self.client,
                        self.identity,
                        Path(directory) / "missing",
                        "file",
                        manifest,
                        content_type=content_type,
                        permissions="public-read",
                        visibility="public",
                        optional=True,
                    )
                self.assertEqual([], json.loads(manifest.read_text()))
        self.client.head_object.assert_not_called()
        self.client.put_object.assert_not_called()
        self.client.create_multipart_upload.assert_not_called()

    def test_allowed_content_types_are_served_as_attachments(self):
        for content_type in [
            "application/octet-stream",
            "application/gzip",
            "application/x-gzip",
            "application/tar",
            "application/x-tar",
            "application/zip",
            "application/json",
            "application/yaml",
            "text/plain",
            "text/yaml",
        ]:
            for permissions, visibility in [("private", "signed"), ("public-read", "public")]:
                for part_size in [4, 64]:
                    with (
                        self.subTest(
                            content_type=content_type, permissions=permissions, part_size=part_size
                        ),
                        tempfile.TemporaryDirectory() as directory,
                        patch("buildscripts.task_artifacts.PART_SIZE", part_size),
                    ):
                        self.client.reset_mock()
                        self.client.create_multipart_upload.return_value = {"UploadId": "upload"}
                        self.client.upload_part.return_value = {"ETag": "part"}
                        local = Path(directory) / "file"
                        local.write_bytes(b"<script>alert('active content')</script>")
                        manifest = Path(directory) / "artifact.json"
                        upload_artifact(
                            self.client,
                            self.identity,
                            local,
                            "file",
                            manifest,
                            content_type=content_type,
                            permissions=permissions,
                            visibility=visibility,
                        )
                        request = (
                            self.client.create_multipart_upload
                            if part_size == 4
                            else self.client.put_object
                        )
                        self.assertEqual(content_type, request.call_args.kwargs["ContentType"])
                        self.assertEqual(
                            "attachment", request.call_args.kwargs["ContentDisposition"]
                        )
                        self.assertEqual(
                            content_type, json.loads(manifest.read_text())[0]["content_type"]
                        )

    def test_multipart_completion_is_conditional(self):
        self.client.create_multipart_upload.return_value = {"UploadId": "upload"}
        self.client.upload_part.side_effect = [{"ETag": "first"}, {"ETag": "second"}]
        with tempfile.TemporaryDirectory() as directory:
            local = Path(directory) / "file"
            local.write_bytes(b"abcdefgh")
            with patch("buildscripts.task_artifacts.PART_SIZE", 4):
                upload_artifact(
                    self.client, self.identity, local, "file", Path(directory) / "artifact.json"
                )
        kwargs = self.client.complete_multipart_upload.call_args.kwargs
        self.assertEqual("*", kwargs["IfNoneMatch"])
        self.assertEqual(
            [{"ETag": "first", "PartNumber": 1}, {"ETag": "second", "PartNumber": 2}],
            kwargs["MultipartUpload"]["Parts"],
        )
        self.client.put_object.assert_not_called()
        self.client.abort_multipart_upload.assert_not_called()

    def test_failed_multipart_upload_is_aborted(self):
        self.client.create_multipart_upload.return_value = {"UploadId": "upload"}
        self.client.upload_part.return_value = {"ETag": "part"}
        self.client.complete_multipart_upload.side_effect = ClientError(
            {"Error": {"Code": "PreconditionFailed"}}, "CompleteMultipartUpload"
        )
        with tempfile.TemporaryDirectory() as directory:
            local = Path(directory) / "file"
            local.write_bytes(b"abcdefgh")
            manifest = Path(directory) / "artifact.json"
            with patch("buildscripts.task_artifacts.PART_SIZE", 4), self.assertRaises(ClientError):
                upload_artifact(self.client, self.identity, local, "file", manifest)
            self.assertEqual([], json.loads(manifest.read_text()))
        self.client.abort_multipart_upload.assert_called_once_with(
            Bucket="mciuploads",
            Key=self.identity.key("file"),
            UploadId="upload",
        )

    def test_restart_condition_is_applied_to_multipart_completion(self):
        self.client.head_object.side_effect = None
        self.client.head_object.return_value = self.existing_object()
        self.client.create_multipart_upload.return_value = {"UploadId": "upload"}
        self.client.upload_part.return_value = {"ETag": "part"}
        with tempfile.TemporaryDirectory() as directory:
            local = Path(directory) / "file"
            local.write_bytes(b"abcdefgh")
            with patch("buildscripts.task_artifacts.PART_SIZE", 4):
                upload_artifact(
                    self.client, self.identity, local, "file", Path(directory) / "artifact.json"
                )
        kwargs = self.client.complete_multipart_upload.call_args.kwargs
        self.assertEqual('"existing-etag"', kwargs["IfMatch"])
        self.assertNotIn("IfNoneMatch", kwargs)
        self.assertNotIn("IfMatch", self.client.create_multipart_upload.call_args.kwargs)

    def test_conflicting_multipart_replacement_is_aborted(self):
        self.client.head_object.side_effect = None
        self.client.head_object.return_value = self.existing_object()
        self.client.create_multipart_upload.return_value = {"UploadId": "upload"}
        self.client.upload_part.return_value = {"ETag": "part"}
        self.client.complete_multipart_upload.side_effect = ClientError(
            {"Error": {"Code": "PreconditionFailed"}}, "CompleteMultipartUpload"
        )
        with tempfile.TemporaryDirectory() as directory:
            local = Path(directory) / "file"
            local.write_bytes(b"abcdefgh")
            manifest = Path(directory) / "artifact.json"
            with patch("buildscripts.task_artifacts.PART_SIZE", 4), self.assertRaises(ClientError):
                upload_artifact(self.client, self.identity, local, "file", manifest)
            self.assertEqual([], json.loads(manifest.read_text()))
        self.assertEqual(
            '"existing-etag"', self.client.complete_multipart_upload.call_args.kwargs["IfMatch"]
        )
        self.client.abort_multipart_upload.assert_called_once_with(
            Bucket="mciuploads", Key=self.identity.key("file"), UploadId="upload"
        )

    def test_conditional_requests_match_the_installed_s3_api(self):
        session = Session()
        # A stubbed client must not load the Evergreen host's AWS profile or configuration.
        session.set_config_variable("profile", "default")
        session.set_config_variable("config_file", "")
        session.set_config_variable("credentials_file", "")
        session.get_component("config_store").set_config_variable("profile", None)
        client = session.create_client(
            "s3", region_name="us-east-1", aws_access_key_id="test", aws_secret_access_key="test"
        )
        key = self.identity.key("file")
        params = {
            "Bucket": "mciuploads",
            "Key": key,
            "ACL": "private",
            "StorageClass": "INTELLIGENT_TIERING",
            "ContentType": "application/octet-stream",
            "ContentDisposition": "attachment",
            "Metadata": ANY,
        }
        with tempfile.TemporaryDirectory() as directory, Stubber(client) as stub:
            local = Path(directory) / "file"
            local.write_bytes(b"abcdefgh")
            stub.add_client_error(
                "head_object", "404", expected_params={"Bucket": "mciuploads", "Key": key}
            )
            stub.add_response("put_object", {}, {**params, "Body": ANY, "IfNoneMatch": "*"})
            upload_artifact(client, self.identity, local, "file", Path(directory) / "artifact.json")
            stub.add_response(
                "head_object", self.existing_object(), {"Bucket": "mciuploads", "Key": key}
            )
            stub.add_response(
                "put_object", {}, {**params, "Body": ANY, "IfMatch": '"existing-etag"'}
            )
            upload_artifact(client, self.identity, local, "file", Path(directory) / "artifact.json")
            upload_params = {"Bucket": "mciuploads", "Key": key, "UploadId": "upload"}
            stub.add_response(
                "head_object", self.existing_object(), {"Bucket": "mciuploads", "Key": key}
            )
            stub.add_response("create_multipart_upload", {"UploadId": "upload"}, params)
            for number in [1, 2]:
                stub.add_response(
                    "upload_part",
                    {"ETag": str(number)},
                    {
                        **upload_params,
                        "PartNumber": number,
                        "Body": ANY,
                    },
                )
            stub.add_response(
                "complete_multipart_upload",
                {},
                {
                    **upload_params,
                    "IfMatch": '"existing-etag"',
                    "MultipartUpload": {
                        "Parts": [{"ETag": "1", "PartNumber": 1}, {"ETag": "2", "PartNumber": 2}]
                    },
                },
            )
            with patch("buildscripts.task_artifacts.PART_SIZE", 4):
                upload_artifact(
                    client, self.identity, local, "file", Path(directory) / "artifact.json"
                )
            stub.assert_no_pending_responses()


if __name__ == "__main__":
    unittest.main()
