"""Tests for task-owned, conditional S3 artifact uploads."""

import io
import json
import os
import stat
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path
from typing import Any
from unittest.mock import Mock, patch

from botocore.exceptions import ClientError, ReadTimeoutError
from botocore.session import Session
from botocore.stub import ANY, Stubber

from buildscripts.task_artifacts import (
    TaskIdentity,
    _parse_release_inputs,
    _replace_release_input,
    _restore_release_inputs,
    is_release_build,
    main,
    upload_artifact,
)


class TaskArtifactsTest(unittest.TestCase):
    def test_workdir_relative_sidecars_and_related_paths_with_spaces(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            inputs = _parse_release_inputs(
                ["provenance=src/provenance.json", "execution-log=src/log"],
                '["src/a/file with spaces", "src/b/file with spaces", "src/provenance"]',
                root,
            )
            self.assertEqual(root / "src/provenance.json", inputs["provenance"])
            self.assertEqual(root / "src/log", inputs["execution-log"])
            self.assertEqual(root / "src/a/file with spaces", inputs["related-0"])
            self.assertEqual(root / "src/b/file with spaces", inputs["related-1"])
            self.assertEqual(root / "src/provenance", inputs["related-2"])

    def test_upload_shell_passes_workdir_and_sidecars_without_splitting(self):
        script = Path(__file__).resolve().parents[2] / "evergreen/upload_task_artifact.sh"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "src").mkdir()
            (root / "venv/bin").mkdir(parents=True)
            executable = root / "venv/bin/python"
            executable.write_text(
                '#!/bin/bash\nprintf "%s\\0" "$@" > "$CAPTURE_ARGS"\n', newline="\n"
            )
            executable.chmod(0o755)
            shell_workdir = subprocess.check_output(
                ["bash", "-c", "pwd"], cwd=root, text=True
            ).strip()
            related = '["src/a/file with spaces", "src/b/file with spaces"]'
            subprocess.run(
                ["bash", str(script)],
                cwd=root,
                env={
                    **os.environ,
                    "OS": "Linux",
                    # The mock runs in src. A relative path also works in Cygwin when the
                    # unittest process uses native Windows paths.
                    "CAPTURE_ARGS": "../args",
                    "artifact_local_file": "src/archive",
                    "artifact_relative_name": "binary",
                    "artifact_release_provenance_file": "src/provenance.json",
                    "artifact_release_execution_log_file": "src/log",
                    "artifact_release_related_files": related,
                },
                check=True,
            )
            args = (root / "args").read_bytes().decode().split("\0")
            self.assertIn(f"--release-input-root={shell_workdir}", args)
            self.assertIn("--release-input=provenance=src/provenance.json", args)
            self.assertIn(f"--release-related-files={related}", args)

    def test_release_checkpoint_checks_free_space_before_download(self):
        self._mock_checkpoint_reads({**self.existing_object(), "ContentLength": 10240})
        with (
            tempfile.TemporaryDirectory() as directory,
            patch("buildscripts.task_artifacts.shutil.disk_usage", return_value=Mock(free=0)),
        ):
            archive = Path(directory) / "archive"
            archive.write_bytes(b"current")
            with self.assertRaisesRegex(ValueError, "Insufficient free space"):
                _restore_release_inputs(self.client, self.identity, archive, "binary", {})
            self.assertEqual(b"current", archive.read_bytes())
        self.client.get_object.assert_not_called()

    def test_checkpoint_requires_configured_provenance_and_log(self):
        for name in ("provenance", "execution-log"):
            for directory_input in (False, True):
                with self.subTest(name=name, directory=directory_input):
                    with tempfile.TemporaryDirectory() as directory:
                        root = Path(directory)
                        archive = root / "archive"
                        archive.write_bytes(b"binary")
                        sidecar = root / "sidecar"
                        if directory_input:
                            sidecar.mkdir()
                        with self.assertRaisesRegex(ValueError, "required release input"):
                            _restore_release_inputs(
                                self.client, self.identity, archive, "binary", {name: sidecar}
                            )
                        self.client.put_object.assert_not_called()

    def test_invalid_checkpoints_leave_all_local_inputs_untouched(self):
        for invalid in (
            "symlink",
            "duplicate",
            "missing-provenance",
            "compressed",
            "oversized",
            "truncated",
        ):
            with self.subTest(invalid=invalid), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                files = {name: root / name for name in ("archive", "provenance", "optional")}
                for name, path in files.items():
                    path.write_bytes(name.encode())
                data = io.BytesIO()
                with tarfile.open(
                    fileobj=data, mode="w:gz" if invalid == "compressed" else "w"
                ) as bundle:
                    names = (
                        ["archive"]
                        if invalid == "missing-provenance"
                        else ["archive", "provenance"]
                    )
                    if invalid == "duplicate":
                        names.append("archive")
                    for name in names:
                        member = tarfile.TarInfo(name)
                        member.size = 3
                        if invalid == "symlink" and name == "provenance":
                            member.type = tarfile.SYMTYPE
                            member.linkname = "optional"
                            member.size = 0
                        bundle.addfile(member, io.BytesIO(b"old"))
                payload = data.getvalue()
                if invalid == "truncated":
                    payload = payload[:513]
                self._mock_checkpoint_reads(
                    {
                        **self.existing_object(),
                        "ContentLength": len(payload),
                    }
                )
                self.client.get_object.return_value = {"Body": io.BytesIO(payload)}
                with patch(
                    "buildscripts.task_artifacts.MAX_CHECKPOINT_SIZE",
                    1 if invalid == "oversized" else 1024**2,
                ):
                    with self.assertRaises((ValueError, tarfile.TarError)):
                        _restore_release_inputs(
                            self.client,
                            self.identity,
                            files["archive"],
                            "binary",
                            {name: path for name, path in files.items() if name != "archive"},
                        )
                for name, path in files.items():
                    self.assertEqual(name.encode(), path.read_bytes())

    def test_preservation_reapplies_requested_acl_before_attaching(self):
        self.client.head_object.side_effect = None
        self.client.head_object.return_value = {**self.existing_object(), "ContentLength": 3}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            local = root / "file"
            local.write_bytes(b"new")
            upload_artifact(
                self.client, self.identity, local, "file", root / "manifest", preserve_existing=True
            )
            self.client.put_object_acl.assert_called_once_with(
                Bucket="mciuploads", Key=self.identity.key("file"), ACL="private"
            )
            self.client.put_object_acl.side_effect = ClientError(
                {"Error": {"Code": "AccessDenied"}}, "PutObjectAcl"
            )
            with self.assertRaises(ClientError):
                upload_artifact(
                    self.client,
                    self.identity,
                    local,
                    "file",
                    root / "manifest",
                    preserve_existing=True,
                )
            self.assertEqual([], json.loads((root / "manifest").read_text()))

    def test_cli_rejects_duplicate_and_reserved_related_names(self):
        for inputs in (("same=a", "same=b"), ("archive=a",), ("provenance=a", "provenance=b")):
            with (
                self.subTest(inputs=inputs),
                patch(
                    "sys.argv",
                    [
                        "task_artifacts",
                        "--local-file=archive",
                        "--relative-name=binary",
                        "--checkpoint-release-inputs",
                        *(f"--release-input={value}" for value in inputs),
                    ],
                ),
                patch("buildscripts.task_artifacts.get_s3_client") as client,
            ):
                with self.assertLogs("buildscripts.task_artifacts", level="ERROR"):
                    self.assertEqual(1, main())
                client.assert_not_called()

    def test_ibm_release_policy_uses_publishing_bucket_without_inherited_marker(self):
        import yaml

        config = (
            Path(__file__).resolve().parents[2]
            / "etc/evergreen_yml_components/variants/ibm/test_release.yml"
        )
        variants = yaml.safe_load(config.read_text())["buildvariants"]
        dynamic = [variant for variant in variants if variant["name"].endswith("-dynamic")]
        self.assertEqual(6, len(dynamic))
        for variant in variants:
            with self.subTest(variant=variant["name"]):
                expansions = variant["expansions"]
                self.assertNotIn("artifact_release_build", expansions)
                self.assertEqual(
                    not variant["name"].endswith("-dynamic"),
                    is_release_build(
                        {
                            "project": "mongodb-mongo-master-nightly",
                            "push_bucket": expansions["push_bucket"],
                        }
                    ),
                )

    def test_release_policy_covers_included_and_anchored_variants(self):
        for env in (
            {"artifact_release_build": "true"},
            {"is_release": "true"},
            {"project": "mongo-release-v9.0"},
            {"project": "mongodb-mongo-master-nightly", "push_bucket": "downloads.10gen.com"},
            {"project": "mongodb-mongo-v9.0", "push_bucket": "origin-mongodb-server-latest"},
        ):
            with self.subTest(env=env):
                self.assertTrue(is_release_build(env))
        self.assertFalse(
            is_release_build({"project": "mongodb-mongo-master", "push_bucket": "bucket"})
        )
        self.assertFalse(is_release_build({"project": "mongodb-mongo-master-nightly"}))

    def test_partial_release_restart_restores_original_archive_and_sidecars(self):
        self._check_partial_release_restart("404")

    def test_windows_release_recovery_replaces_read_only_bazel_output(self):
        objects = self._mock_release_store()
        real_replace = os.replace

        def windows_replace(source, destination):
            path = Path(destination)
            if path.exists() and not path.stat().st_mode & stat.S_IWRITE:
                raise PermissionError("Windows cannot replace a read-only destination")
            return real_replace(source, destination)

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive, provenance = root / "archive", root / "provenance"
            archive.write_bytes(b"")
            with (
                patch("buildscripts.task_artifacts.os", wraps=os) as windows_os,
                patch("os.replace", side_effect=windows_replace),
            ):
                windows_os.name = "nt"
                for binary, proof in (
                    (b"first binary", b"first proof"),
                    (b"rebuilt binary", b"rebuilt proof"),
                ):
                    archive.chmod(stat.S_IREAD | stat.S_IWRITE)
                    archive.write_bytes(binary)
                    provenance.write_bytes(proof)
                    archive.chmod(stat.S_IREAD)
                    upload_artifact(
                        self.client,
                        self.identity,
                        archive,
                        "binary",
                        root / "manifest",
                        preserve_existing=True,
                        release_inputs={"provenance": provenance},
                    )
                    self.assertEqual(b"first binary", archive.read_bytes())
                    self.assertEqual(b"first proof", provenance.read_bytes())
                    self.assertFalse(archive.stat().st_mode & stat.S_IWRITE)
                    self.assertEqual(1, len(json.loads((root / "manifest").read_text())))
            self.assertEqual(b"first binary", objects[self.identity.key("binary")]["Body"])

    def test_windows_failed_replacement_retains_original_read_only_input(self):
        with tempfile.TemporaryDirectory() as directory:
            destination, pending = Path(directory) / "binary", Path(directory) / "pending"
            destination.write_bytes(b"current bytes")
            destination.chmod(stat.S_IREAD)
            pending.write_bytes(b"checkpoint bytes")
            with (
                patch("buildscripts.task_artifacts.os", wraps=os) as windows_os,
                patch("os.replace", side_effect=PermissionError("file is locked")) as replace,
            ):
                windows_os.name = "nt"
                with self.assertRaises(PermissionError):
                    _replace_release_input(pending, destination)
            self.assertEqual(2, replace.call_count)
            self.assertEqual(b"current bytes", destination.read_bytes())
            self.assertFalse(destination.stat().st_mode & stat.S_IWRITE)
            self.assertEqual(b"checkpoint bytes", pending.read_bytes())

    def test_windows_unrelated_access_failure_does_not_change_permissions(self):
        with tempfile.TemporaryDirectory() as directory:
            destination, pending = Path(directory) / "binary", Path(directory) / "pending"
            destination.write_bytes(b"current bytes")
            pending.write_bytes(b"checkpoint bytes")
            mode = destination.stat().st_mode
            with (
                patch("buildscripts.task_artifacts.os", wraps=os) as windows_os,
                patch(
                    "os.replace", side_effect=PermissionError("directory access denied")
                ) as replace,
            ):
                windows_os.name = "nt"
                with self.assertRaises(PermissionError):
                    _replace_release_input(pending, destination)
            replace.assert_called_once()
            self.assertEqual(mode, destination.stat().st_mode)
            self.assertEqual(b"current bytes", destination.read_bytes())

    def test_ordinary_publishing_patches_do_not_require_release_inputs(self):
        for project in ("mongodb-mongo-master-nightly", "mongodb-mongo-v9.0", "mongo-release"):
            with self.subTest(project=project), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                archive = root / "archive"
                archive.write_bytes(b"patch binary")
                env = {
                    "project": project,
                    "build_variant": "variant",
                    "version_id": "version",
                    "task_name": "crypt_create_lib",
                    "task_id": "task",
                    "execution": "0",
                    "is_patch": "true",
                    "is_release": "false",
                    "push_bucket": "downloads.10gen.com",
                }
                self.client.reset_mock()
                with (
                    patch.dict(os.environ, env, clear=True),
                    patch("buildscripts.task_artifacts.get_s3_client", return_value=self.client),
                    patch(
                        "sys.argv",
                        [
                            "task_artifacts",
                            f"--local-file={archive}",
                            "--relative-name=binary",
                            f"--manifest={root / 'manifest'}",
                            "--checkpoint-release-inputs",
                            f"--release-input=execution-log={root / 'missing-log'}",
                        ],
                    ),
                ):
                    self.assertEqual(0, main())
                self.client.put_object.assert_called_once()
                self.assertFalse(
                    self.client.put_object.call_args.kwargs["Key"].endswith(".release-inputs.tar")
                )
                self.assertEqual(1, len(json.loads((root / "manifest").read_text())))

    def test_explicit_release_markers_override_patch_status(self):
        for marker in ("is_release", "artifact_release_build"):
            with self.subTest(marker=marker):
                self.assertTrue(
                    is_release_build(
                        {
                            "project": "mongodb-mongo-master-nightly",
                            "is_patch": "true",
                            marker: "true",
                        }
                    )
                )

    def test_partial_release_restart_with_denied_missing_head_restores_original_inputs(self):
        self._check_partial_release_restart("403")

    def test_release_creation_uses_conditional_writes_without_listing(self):
        for part_size in (64, 4):
            with self.subTest(part_size=part_size), tempfile.TemporaryDirectory() as directory:
                self.client.reset_mock()
                self.client.head_object.side_effect = ClientError(
                    {"Error": {"Code": "403"}}, "HeadObject"
                )
                self.client.list_objects_v2.side_effect = AssertionError(
                    "Bucket listing is forbidden"
                )
                self.client.create_multipart_upload.return_value = {"UploadId": "upload"}
                self.client.upload_part.return_value = {"ETag": "part"}
                root = Path(directory)
                local = root / "file"
                local.write_bytes(b"original provenance")
                with patch("buildscripts.task_artifacts.PART_SIZE", part_size):
                    upload_artifact(
                        self.client,
                        self.identity,
                        local,
                        "provenance",
                        root / "manifest",
                        preserve_existing=True,
                    )
                write = (
                    self.client.put_object
                    if part_size == 64
                    else self.client.complete_multipart_upload
                )
                self.assertEqual("*", write.call_args.kwargs["IfNoneMatch"])
        self.client.list_objects_v2.assert_not_called()

    def _mock_checkpoint_reads(self, existing: dict[str, Any]) -> None:
        def head_object(**params):
            if params["Key"].endswith(".release-inputs.tar"):
                return existing
            raise ClientError({"Error": {"Code": "404"}}, "HeadObject")

        self.client.head_object.side_effect = head_object

    def _mock_release_store(self, missing_code: str = "403") -> dict[str, Any]:
        objects = {}

        def head_object(**params):
            if params["Key"] not in objects:
                raise ClientError({"Error": {"Code": missing_code}}, "HeadObject")
            obj = objects[params["Key"]]
            return {**obj, "ETag": '"etag"', "ContentLength": len(obj["Body"])}

        def put_object(**params):
            self.assertEqual("*", params["IfNoneMatch"])
            if params["Key"] in objects:
                raise ClientError({"Error": {"Code": "PreconditionFailed"}}, "PutObject")
            objects[params["Key"]] = {**params, "Body": params["Body"].read()}

        def get_object(**params):
            self.assertEqual('"etag"', params["IfMatch"])
            return {**head_object(**params), "Body": io.BytesIO(objects[params["Key"]]["Body"])}

        self.client.head_object.side_effect = head_object
        self.client.put_object.side_effect = put_object
        self.client.get_object.side_effect = get_object
        self.client.list_objects_v2.side_effect = AssertionError("Bucket listing is forbidden")
        return objects

    def _check_partial_release_restart(self, missing_code: str):
        objects = self._mock_release_store(missing_code)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "binary.tgz"
            provenance = root / "provenance.json"
            execution_log = root / "execution-log"
            absent = root / "optional-archive"
            inputs = {"provenance": provenance, "execution-log": execution_log, "optional": absent}
            archive.write_bytes(b"original binary")
            provenance.write_bytes(b"original provenance")
            execution_log.write_bytes(b"original execution log")
            upload_artifact(
                self.client,
                self.identity,
                archive,
                "binary",
                root / "manifest",
                preserve_existing=True,
                release_inputs=inputs,
            )
            # Simulate a failure before the ordinary provenance/log uploads, followed by a rebuild.
            archive.write_bytes(b"rebuilt binary")
            provenance.write_bytes(b"rebuilt provenance")
            execution_log.write_bytes(b"rebuilt execution log")
            absent.write_bytes(b"new optional input")
            retry = TaskIdentity("project", "variant", "version", "archive", "task-id", "1")
            upload_artifact(
                self.client,
                retry,
                archive,
                "binary",
                root / "manifest",
                preserve_existing=True,
                release_inputs=inputs,
            )
            self.assertEqual(b"original binary", archive.read_bytes())
            self.assertEqual(b"original provenance", provenance.read_bytes())
            self.assertEqual(b"original execution log", execution_log.read_bytes())
            self.assertFalse(absent.exists())
            self.assertEqual(2, self.client.put_object.call_count)
            self.assertEqual(
                '"etag"', objects[retry.key("binary")]["Metadata"].get("release-checkpoint-etag")
            )
            self.client.list_objects_v2.assert_not_called()
            # The interrupted sidecar uploads now publish the checkpoint's original bytes.
            for name, path in inputs.items():
                upload_artifact(
                    self.client,
                    retry,
                    path,
                    name,
                    root / "manifest",
                    preserve_existing=True,
                    optional=True,
                )
            self.assertEqual(b"original provenance", objects[retry.key("provenance")]["Body"])
            self.assertEqual(b"original execution log", objects[retry.key("execution-log")]["Body"])
            # Already published provenance and logs have the same overwrite protection as binaries.
            for name, path in inputs.items():
                if not path.exists():
                    continue
                path.write_bytes(b"another rebuild")
                upload_artifact(
                    self.client,
                    retry,
                    path,
                    name,
                    root / "manifest",
                    preserve_existing=True,
                )
            self.assertEqual(b"original provenance", objects[retry.key("provenance")]["Body"])
            self.assertEqual(b"original execution log", objects[retry.key("execution-log")]["Body"])

    def test_incomplete_multipart_checkpoint_publishes_nothing_and_retry_uses_new_build(self):
        objects = self._mock_release_store()
        self.client.create_multipart_upload.return_value = {"UploadId": "upload"}
        self.client.upload_part.return_value = {"ETag": "part"}
        self.client.complete_multipart_upload.side_effect = ClientError(
            {"Error": {"Code": "InternalError"}}, "CompleteMultipartUpload"
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive, provenance = root / "binary", root / "provenance"
            archive.write_bytes(b"first binary")
            provenance.write_bytes(b"first provenance")
            with (
                patch("buildscripts.task_artifacts.PART_SIZE", 4096),
                self.assertRaises(ClientError),
            ):
                upload_artifact(
                    self.client,
                    self.identity,
                    archive,
                    "binary",
                    root / "manifest",
                    preserve_existing=True,
                    release_inputs={"provenance": provenance},
                )
            self.assertEqual({}, objects)
            self.assertEqual([], json.loads((root / "manifest").read_text()))
            self.client.abort_multipart_upload.assert_called_once()
            self.client.get_object.assert_not_called()
            archive.write_bytes(b"retry binary")
            provenance.write_bytes(b"retry provenance")
            upload_artifact(
                self.client,
                self.identity,
                archive,
                "binary",
                root / "manifest",
                preserve_existing=True,
                release_inputs={"provenance": provenance},
            )
            self.assertEqual(b"retry binary", objects[self.identity.key("binary")]["Body"])
            self.assertEqual(b"retry provenance", provenance.read_bytes())

    def test_completed_checkpoint_with_lost_response_restores_original_inputs(self):
        objects = self._mock_release_store()
        store_object = self.client.put_object.side_effect

        def lose_response(**params):
            store_object(**params)
            raise ReadTimeoutError(endpoint_url="https://mciuploads.s3.amazonaws.com")

        self.client.put_object.side_effect = lose_response
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive, provenance = root / "binary", root / "provenance"
            archive.write_bytes(b"original binary")
            provenance.write_bytes(b"original provenance")
            with self.assertRaises(ReadTimeoutError):
                upload_artifact(
                    self.client,
                    self.identity,
                    archive,
                    "binary",
                    root / "manifest",
                    preserve_existing=True,
                    release_inputs={"provenance": provenance},
                )
            self.assertEqual([self.identity.key("binary.release-inputs.tar")], list(objects))
            self.assertEqual([], json.loads((root / "manifest").read_text()))
            self.client.put_object.side_effect = store_object
            archive.write_bytes(b"rebuilt binary")
            provenance.write_bytes(b"rebuilt provenance")
            upload_artifact(
                self.client,
                self.identity,
                archive,
                "binary",
                root / "manifest",
                preserve_existing=True,
                release_inputs={"provenance": provenance},
            )
            self.assertEqual(b"original binary", objects[self.identity.key("binary")]["Body"])
            self.assertEqual(b"original provenance", provenance.read_bytes())

    def test_checkpoint_creation_race_restores_winning_bundle_before_archive_upload(self):
        objects = self._mock_release_store()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive, provenance = root / "binary", root / "provenance"
            archive.write_bytes(b"winning binary")
            provenance.write_bytes(b"winning provenance")
            upload_artifact(
                self.client,
                self.identity,
                archive,
                "binary",
                root / "manifest",
                preserve_existing=True,
                release_inputs={"provenance": provenance},
            )
            # The competing execution has committed its checkpoint, but not its binary.
            del objects[self.identity.key("binary")]
            checkpoint_key = self.identity.key("binary.release-inputs.tar")
            checkpoint_bytes = objects[checkpoint_key]["Body"]
            readable_head = self.client.head_object.side_effect
            denied_reads = 0

            def head_object(**params):
                nonlocal denied_reads
                if params["Key"] == checkpoint_key and denied_reads < 2:
                    denied_reads += 1
                    raise ClientError({"Error": {"Code": "403"}}, "HeadObject")
                return readable_head(**params)

            self.client.head_object.side_effect = head_object
            archive.write_bytes(b"losing binary")
            provenance.write_bytes(b"losing provenance")
            upload_artifact(
                self.client,
                self.identity,
                archive,
                "binary",
                root / "manifest",
                preserve_existing=True,
                release_inputs={"provenance": provenance},
            )
            self.assertEqual(checkpoint_bytes, objects[checkpoint_key]["Body"])
            self.assertEqual(b"winning binary", objects[self.identity.key("binary")]["Body"])
            self.assertEqual(b"winning provenance", provenance.read_bytes())
            self.assertEqual(
                '"etag"',
                objects[self.identity.key("binary")]["Metadata"]["release-checkpoint-etag"],
            )
            self.client.list_objects_v2.assert_not_called()

    def test_unreadable_checkpoint_fails_after_conditional_refusal_without_publishing(self):
        self.client.head_object.side_effect = ClientError({"Error": {"Code": "403"}}, "HeadObject")
        self.client.put_object.side_effect = ClientError({"Error": {"Code": "412"}}, "PutObject")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "binary"
            archive.write_bytes(b"current binary")
            with self.assertRaises(ClientError):
                upload_artifact(
                    self.client,
                    self.identity,
                    archive,
                    "binary",
                    root / "manifest",
                    preserve_existing=True,
                    release_inputs={},
                )
            self.assertEqual(b"current binary", archive.read_bytes())
            self.assertEqual([], json.loads((root / "manifest").read_text()))
            self.client.put_object.assert_called_once()
            self.assertEqual(
                self.identity.key("binary.release-inputs.tar"),
                self.client.put_object.call_args.kwargs["Key"],
            )
            self.assertEqual("*", self.client.put_object.call_args.kwargs["IfNoneMatch"])
            self.client.get_object.assert_not_called()
            self.client.put_object_acl.assert_not_called()
            self.client.list_objects_v2.assert_not_called()

    def test_wrong_checkpoint_link_leaves_rebuild_inputs_untouched(self):
        for checkpoint_etag in (None, '"different-checkpoint"'):
            with self.subTest(etag=checkpoint_etag), tempfile.TemporaryDirectory() as directory:
                objects = self._mock_release_store()
                root = Path(directory)
                archive, provenance = root / "binary", root / "provenance"
                archive.write_bytes(b"original binary")
                provenance.write_bytes(b"original provenance")
                upload_artifact(
                    self.client,
                    self.identity,
                    archive,
                    "binary",
                    root / "manifest",
                    preserve_existing=True,
                    release_inputs={"provenance": provenance},
                )
                metadata = objects[self.identity.key("binary")]["Metadata"]
                metadata["release-checkpoint-etag"] = checkpoint_etag
                archive.write_bytes(b"rebuilt binary")
                provenance.write_bytes(b"rebuilt provenance")
                writes = self.client.put_object.call_count
                with self.assertRaisesRegex(ValueError, "not linked to this recovery checkpoint"):
                    upload_artifact(
                        self.client,
                        self.identity,
                        archive,
                        "binary",
                        root / "manifest",
                        preserve_existing=True,
                        release_inputs={"provenance": provenance},
                    )
                self.assertEqual(b"rebuilt binary", archive.read_bytes())
                self.assertEqual(b"rebuilt provenance", provenance.read_bytes())
                self.assertEqual(writes, self.client.put_object.call_count)
                self.assertEqual([], json.loads((root / "manifest").read_text()))

    def test_archive_creation_race_requires_matching_checkpoint_link(self):
        for linked in (True, False):
            with self.subTest(linked=linked), tempfile.TemporaryDirectory() as directory:
                objects = self._mock_release_store()
                store_object = self.client.put_object.side_effect

                def race(**params):
                    store_object(**params)
                    if params["Key"] == self.identity.key("binary"):
                        if not linked:
                            del objects[params["Key"]]["Metadata"]["release-checkpoint-etag"]
                        raise ClientError({"Error": {"Code": "PreconditionFailed"}}, "PutObject")

                self.client.put_object.side_effect = race
                root = Path(directory)
                archive, provenance = root / "binary", root / "provenance"
                archive.write_bytes(b"binary")
                provenance.write_bytes(b"provenance")
                if linked:
                    upload_artifact(
                        self.client,
                        self.identity,
                        archive,
                        "binary",
                        root / "manifest",
                        preserve_existing=True,
                        release_inputs={"provenance": provenance},
                    )
                    self.assertEqual(1, len(json.loads((root / "manifest").read_text())))
                else:
                    with self.assertRaisesRegex(
                        ValueError, "not linked to this recovery checkpoint"
                    ):
                        upload_artifact(
                            self.client,
                            self.identity,
                            archive,
                            "binary",
                            root / "manifest",
                            preserve_existing=True,
                            release_inputs={"provenance": provenance},
                        )
                    self.assertEqual([], json.loads((root / "manifest").read_text()))

    def test_unreadable_archive_cannot_be_relabelled_with_new_checkpoint(self):
        objects = self._mock_release_store()
        key = self.identity.key("binary")
        objects[key] = {**self.existing_object(), "Body": b"old uncheckpointed binary"}
        readable_head = self.client.head_object.side_effect

        def head_object(**params):
            if params["Key"] == key:
                raise ClientError({"Error": {"Code": "403"}}, "HeadObject")
            return readable_head(**params)

        self.client.head_object.side_effect = head_object
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive, provenance = root / "binary", root / "provenance"
            archive.write_bytes(b"rebuilt binary")
            provenance.write_bytes(b"rebuilt provenance")
            with self.assertRaises(ClientError):
                upload_artifact(
                    self.client,
                    self.identity,
                    archive,
                    "binary",
                    root / "manifest",
                    preserve_existing=True,
                    release_inputs={"provenance": provenance},
                )
            self.assertEqual(b"old uncheckpointed binary", objects[key]["Body"])
            self.assertNotIn("release-checkpoint-etag", objects[key]["Metadata"])
            self.assertEqual(
                "private", objects[self.identity.key("binary.release-inputs.tar")]["ACL"]
            )
            self.assertEqual([], json.loads((root / "manifest").read_text()))
            self.client.put_object_acl.assert_not_called()

    def test_development_upload_does_not_restore_release_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "binary"
            path.write_bytes(b"development rebuild")
            upload_artifact(
                self.client,
                self.identity,
                path,
                "binary",
                Path(directory) / "manifest",
                release_inputs={"missing": Path(directory) / "missing"},
            )
            self.assertEqual(1, self.client.put_object.call_count)
            self.client.get_object.assert_not_called()

    def test_optional_missing_archive_skips_checkpoint_creation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            upload_artifact(
                self.client,
                self.identity,
                root / "missing",
                "binary",
                root / "manifest",
                optional=True,
                preserve_existing=True,
                release_inputs={"provenance": root / "missing-sidecar"},
            )
            self.assertEqual([], json.loads((root / "manifest").read_text()))
        self.client.put_object.assert_not_called()

    def test_release_checkpoint_refuses_to_relabel_an_existing_uncheckpointed_archive(self):
        self.client.head_object.side_effect = [
            ClientError({"Error": {"Code": "404"}}, "HeadObject"),
            {**self.existing_object(), "ContentLength": 5},
        ]
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / "archive"
            archive.write_bytes(b"new bytes")
            with self.assertRaisesRegex(ValueError, "no recovery checkpoint"):
                upload_artifact(
                    self.client,
                    self.identity,
                    archive,
                    "binary",
                    Path(directory) / "manifest",
                    preserve_existing=True,
                    release_inputs={},
                )
        self.client.put_object.assert_not_called()

    def test_release_restart_preserves_existing_bytes_and_attaches_remote_size(self):
        self.client.head_object.side_effect = None
        self.client.head_object.return_value = {**self.existing_object(), "ContentLength": 5}
        with tempfile.TemporaryDirectory() as directory:
            local = Path(directory) / "file"
            local.write_bytes(b"a different rebuilt artifact")
            manifest = Path(directory) / "artifact.json"
            with self.assertLogs("buildscripts.task_artifacts", level="INFO") as logs:
                upload_artifact(
                    self.client, self.identity, local, "file", manifest, preserve_existing=True
                )
            self.assertEqual(5, json.loads(manifest.read_text())[0]["file_size"])
            self.assertIn("Preserved existing release artifact", " ".join(logs.output))
        self.client.put_object.assert_not_called()
        self.client.create_multipart_upload.assert_not_called()

    def test_release_creation_is_conditional_for_small_and_large_files(self):
        for part_size in (64, 4):
            with self.subTest(part_size=part_size), tempfile.TemporaryDirectory() as directory:
                self.client.reset_mock()
                self.client.create_multipart_upload.return_value = {"UploadId": "upload"}
                self.client.upload_part.return_value = {"ETag": "part"}
                local = Path(directory) / "file"
                local.write_bytes(b"abcdefgh")
                with patch("buildscripts.task_artifacts.PART_SIZE", part_size):
                    upload_artifact(
                        self.client,
                        self.identity,
                        local,
                        "file",
                        Path(directory) / "manifest",
                        preserve_existing=True,
                    )
                write = (
                    self.client.put_object
                    if part_size == 64
                    else self.client.complete_multipart_upload
                )
                self.assertEqual("*", write.call_args.kwargs["IfNoneMatch"])
                self.assertNotIn("IfMatch", write.call_args.kwargs)

    def test_release_preservation_rejects_another_tasks_object(self):
        self.client.head_object.side_effect = None
        existing = {**self.existing_object(), "ContentLength": 5}
        existing["Metadata"]["task-id"] = "wrong-producer"
        self.client.head_object.return_value = existing
        with tempfile.TemporaryDirectory() as directory:
            local = Path(directory) / "file"
            local.write_bytes(b"binary")
            manifest = Path(directory) / "manifest"
            with self.assertRaisesRegex(ValueError, "not owned"):
                upload_artifact(
                    self.client, self.identity, local, "file", manifest, preserve_existing=True
                )
            self.assertEqual([], json.loads(manifest.read_text()))
        self.client.put_object.assert_not_called()

    def test_release_racing_creation_preserves_only_after_verifying_ownership(self):
        for owned in (True, False):
            with self.subTest(owned=owned), tempfile.TemporaryDirectory() as directory:
                existing = {**self.existing_object(), "ContentLength": 5}
                if not owned:
                    existing["Metadata"]["task-id"] = "wrong-producer"
                self.client.head_object.side_effect = [
                    ClientError({"Error": {"Code": "404"}}, "HeadObject"),
                    existing,
                ]
                self.client.put_object.side_effect = ClientError(
                    {"Error": {"Code": "PreconditionFailed"}}, "PutObject"
                )
                local = Path(directory) / "file"
                local.write_bytes(b"rebuilt")
                manifest = Path(directory) / "manifest"
                if owned:
                    upload_artifact(
                        self.client, self.identity, local, "file", manifest, preserve_existing=True
                    )
                    self.assertEqual(5, json.loads(manifest.read_text())[0]["file_size"])
                else:
                    with self.assertRaisesRegex(ValueError, "not owned"):
                        upload_artifact(
                            self.client,
                            self.identity,
                            local,
                            "file",
                            manifest,
                            preserve_existing=True,
                        )
                    self.assertEqual([], json.loads(manifest.read_text()))

    def test_release_multipart_race_aborts_and_preserves_the_winning_object(self):
        self.client.head_object.side_effect = [
            ClientError({"Error": {"Code": "404"}}, "HeadObject"),
            {**self.existing_object(), "ContentLength": 5},
        ]
        self.client.create_multipart_upload.return_value = {"UploadId": "upload"}
        self.client.upload_part.return_value = {"ETag": "part"}
        self.client.complete_multipart_upload.side_effect = ClientError(
            {"Error": {"Code": "PreconditionFailed"}}, "CompleteMultipartUpload"
        )
        with tempfile.TemporaryDirectory() as directory:
            local = Path(directory) / "file"
            local.write_bytes(b"abcdefgh")
            manifest = Path(directory) / "manifest"
            with patch("buildscripts.task_artifacts.PART_SIZE", 4):
                upload_artifact(
                    self.client, self.identity, local, "file", manifest, preserve_existing=True
                )
            self.assertEqual(5, json.loads(manifest.read_text())[0]["file_size"])
        self.client.abort_multipart_upload.assert_called_once()
        self.assertEqual("*", self.client.complete_multipart_upload.call_args.kwargs["IfNoneMatch"])

    def test_unreadable_release_object_does_not_silently_succeed(self):
        self.client.head_object.side_effect = ClientError(
            {"Error": {"Code": "AccessDenied"}}, "HeadObject"
        )
        self.client.put_object.side_effect = ClientError(
            {"Error": {"Code": "PreconditionFailed"}}, "PutObject"
        )
        with tempfile.TemporaryDirectory() as directory:
            local = Path(directory) / "file"
            local.write_bytes(b"binary")
            manifest = Path(directory) / "manifest"
            with self.assertRaises(ClientError):
                upload_artifact(
                    self.client, self.identity, local, "file", manifest, preserve_existing=True
                )
            self.assertEqual([], json.loads(manifest.read_text()))

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
