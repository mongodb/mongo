#!/usr/bin/env python3
"""Upload internal build artifacts to a task-owned prefix with conditional writes."""

import argparse
import json
import logging
import os
import re
import shutil
import stat
import tarfile
import tempfile
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path
from typing import Any
from urllib.parse import quote

from botocore.exceptions import ClientError

from buildscripts.util.download_utils import get_s3_client

BUCKET = "mciuploads"
PART_SIZE = 64 * 1024 * 1024
# Release checkpoints include several large binary archives. Bound both download and extraction.
MAX_CHECKPOINT_SIZE = 100 * 1024**3
REQUIRED_RELEASE_INPUTS = frozenset({"provenance", "execution-log", "image-digest"})
CONTENT_TYPES = frozenset(
    {
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
    }
)
_COMPONENT = re.compile(r"[A-Za-z0-9_.-]+\Z")
_EXECUTION = re.compile(r"[0-9]+\Z")
LOGGER = logging.getLogger(__name__)


def is_release_build(env: Mapping[str, str]) -> bool:
    """Cover release producers in included modules and anchored variant templates too."""
    if env.get("artifact_release_build") == "true" or env.get("is_release") == "true":
        return True
    if env.get("is_patch") == "true":
        return False
    return env.get("project", "").startswith("mongo-release") or (
        env.get("project") != "mongodb-mongo-master" and bool(env.get("push_bucket"))
    )


def _validate_component(value: str) -> None:
    if not _COMPONENT.fullmatch(value) or value in {".", ".."}:
        raise ValueError(f"Invalid task artifact path component: {value!r}")


@dataclass(frozen=True)
class TaskIdentity:
    """Validated task expansions, not authenticated identity or an authorization boundary."""

    project: str
    build_variant: str
    version_id: str
    task_name: str
    task_id: str
    execution: str

    def __post_init__(self) -> None:
        for value in vars(self).values():
            _validate_component(value)
        if not _EXECUTION.fullmatch(self.execution):
            raise ValueError(f"Invalid task execution: {self.execution!r}")

    def key(self, relative_name: str) -> str:
        """Construct a key within the supplied identity's prefix and the fixed bucket."""
        for component in relative_name.split("/"):
            _validate_component(component)
        return (
            f"{self.project}/{self.build_variant}/{self.version_id}/"
            f"task-artifacts/{self.task_name}/{relative_name}"
        )


def _write_condition(
    client: Any,
    identity: TaskIdentity,
    key: str,
    *,
    existing: dict[str, Any] | None = None,
) -> dict[str, str]:
    """Allow atomic creation, or replacement after verifying ownership and the observed ETag."""
    try:
        if existing is None:
            existing = client.head_object(Bucket=BUCKET, Key=key)
    except ClientError as error:
        if error.response.get("Error", {}).get("Code") in {
            "404",
            "NoSuchKey",
            "NotFound",
            "403",
            "AccessDenied",
        }:
            # Missing objects return 403 without ListBucket permission. If-None-Match safely
            # permits creation even when HEAD is ambiguous; it cannot overwrite any object.
            return {"IfNoneMatch": "*"}
        raise

    metadata = existing.get("Metadata", {})
    owner = {
        "build-variant": identity.build_variant,
        "task-name": identity.task_name,
        "task-id": identity.task_id,
    }
    if any(metadata.get(name) != value for name, value in owner.items()):
        raise ValueError("Artifact destination exists and is not owned by this task")
    execution = metadata.get("execution", "")
    if not _EXECUTION.fullmatch(execution):
        raise ValueError("Existing artifact has no valid task execution")
    if int(execution) > int(identity.execution):
        raise ValueError("Artifact destination belongs to a newer task execution")
    etag = existing.get("ETag")
    if not etag:
        raise ValueError("Existing artifact has no ETag for a conditional replacement")
    return {"IfMatch": etag}


def _existing_release_artifact(
    client: Any, identity: TaskIdentity, key: str, *, allow_conditional_create: bool = False
) -> dict[str, Any] | None:
    """Verify an immutable artifact, allowing ambiguous HEAD only before conditional creation."""
    try:
        existing = client.head_object(Bucket=BUCKET, Key=key)
    except ClientError as error:
        code = error.response.get("Error", {}).get("Code")
        if code in {
            "404",
            "NoSuchKey",
            "NotFound",
        }:
            return None
        if allow_conditional_create and code in {"403", "AccessDenied"}:
            # Without ListBucket, S3 also returns 403 for missing objects. This is not proof
            # of absence: the caller may only attempt If-None-Match: * creation. A 412 must
            # be followed by a successful read and ownership check, never another creation.
            return None
        raise
    _write_condition(client, identity, key, existing=existing)
    return existing


def _verify_release_checkpoint(
    existing: dict[str, Any] | None, checkpoint_etag: str | None
) -> None:
    if existing is not None and checkpoint_etag is not None:
        if existing.get("Metadata", {}).get("release-checkpoint-etag") != checkpoint_etag:
            raise ValueError(
                "Existing release archive is not linked to this recovery checkpoint; "
                "manual recovery is required"
            )


def upload_artifact(
    client: Any,
    identity: TaskIdentity,
    local_file: Path,
    relative_name: str,
    manifest: Path,
    *,
    display_name: str = "",
    content_type: str = "application/octet-stream",
    permissions: str = "private",
    visibility: str = "signed",
    optional: bool = False,
    preserve_existing: bool = False,
    release_inputs: Mapping[str, Path] | None = None,
) -> None:
    """Conditionally upload a file and write a credential-free Evergreen artifact attachment.

    Optional only permits a missing local file. A task may replace its own artifacts when retrying
    or restarting, unless preserve_existing retains the original release artifact. Objects owned
    by another task, observed newer executions, permission errors,
    and conflicting writes fail the task.
    """
    # Clear a previous invocation's manifest before doing anything that could fail or skip.
    manifest.write_text("[]\n", encoding="utf-8")
    key = identity.key(relative_name)
    if permissions not in {"private", "public-read"}:
        raise ValueError(f"Unsupported artifact permissions: {permissions!r}")
    if visibility not in {"signed", "public"}:
        raise ValueError(f"Unsupported artifact visibility: {visibility!r}")
    if visibility == "signed" and permissions != "private":
        raise ValueError("Signed artifacts must use private permissions")
    if visibility == "public" and permissions != "public-read":
        raise ValueError("Public artifacts must use public-read permissions")
    if content_type not in CONTENT_TYPES:
        raise ValueError(f"Unsupported artifact content type: {content_type!r}")
    checkpoint_etag = None
    if preserve_existing and release_inputs is not None:
        try:
            checkpoint_etag = _restore_release_inputs(
                client, identity, local_file, relative_name, release_inputs
            )
        except FileNotFoundError:
            if optional and not local_file.exists():
                LOGGER.info("Skipping missing optional artifact %s", local_file)
                return
            raise
    try:
        source = local_file.open("rb")
    except FileNotFoundError:
        if optional:
            LOGGER.info("Skipping missing optional artifact %s", local_file)
            return
        raise

    metadata = {
        "build-variant": identity.build_variant,
        "task-name": identity.task_name,
        "task-id": identity.task_id,
        "execution": identity.execution,
    }
    if checkpoint_etag is not None:
        metadata["release-checkpoint-etag"] = checkpoint_etag
    params = {
        "Bucket": BUCKET,
        "Key": key,
        "ACL": permissions,
        "StorageClass": "INTELLIGENT_TIERING",
        "ContentType": content_type,
        # Download even text artifacts; their bytes may otherwise be interpreted as web content.
        "ContentDisposition": "attachment",
        "Metadata": metadata,
    }
    with source:
        size = os.fstat(source.fileno()).st_size
        # S3 permits at most 10,000 parts, each between 5 MiB and 5 GiB (except the last).
        part_size = max(PART_SIZE, (size + 9999) // 10000)
        if part_size > 5 * 1024**3:
            raise ValueError("Artifact exceeds the S3 multipart upload size limit")
        existing = (
            _existing_release_artifact(client, identity, key, allow_conditional_create=True)
            if preserve_existing
            else None
        )
        _verify_release_checkpoint(existing, checkpoint_etag)
        condition = (
            {"IfNoneMatch": "*"} if preserve_existing else _write_condition(client, identity, key)
        )
        preserved = existing is not None
        try:
            if not preserved:
                if size <= PART_SIZE:
                    client.put_object(**params, Body=source, **condition)
                else:
                    upload_id = client.create_multipart_upload(**params)["UploadId"]
                    upload_params = {"Bucket": BUCKET, "Key": key, "UploadId": upload_id}
                    try:
                        parts = []
                        while data := source.read(part_size):
                            number = len(parts) + 1
                            part = client.upload_part(**upload_params, PartNumber=number, Body=data)
                            parts.append({"ETag": part["ETag"], "PartNumber": number})
                        client.complete_multipart_upload(
                            **upload_params, MultipartUpload={"Parts": parts}, **condition
                        )
                    except BaseException:
                        try:
                            client.abort_multipart_upload(**upload_params)
                        except Exception:
                            LOGGER.exception(
                                "Could not abort incomplete upload of s3://%s/%s", BUCKET, key
                            )
                        raise
        except ClientError as error:
            if not preserve_existing or error.response.get("Error", {}).get("Code") not in {
                "PreconditionFailed",
                "412",
            }:
                raise
            # Another execution may have created the release artifact after HEAD. Verify its
            # ownership before treating this conditional-write failure as an idempotent success.
            existing = _existing_release_artifact(client, identity, key)
            if existing is None:
                raise
            _verify_release_checkpoint(existing, checkpoint_etag)
            preserved = True
        if preserved:
            # Preservation keeps bytes, but must still honor the requested access setting before
            # advertising it in Evergreen. This also handles a conditional-creation race.
            client.put_object_acl(Bucket=BUCKET, Key=key, ACL=permissions)
            size = existing["ContentLength"]
            content_type = existing.get("ContentType", content_type)

    artifact = {
        "name": display_name or local_file.name,
        "link": f"https://{BUCKET}.s3.amazonaws.com/{quote(key, safe='/')}",
        "visibility": visibility,
        "bucket": BUCKET,
        "filekey": key,
        "content_type": content_type,
        "file_size": size,
    }
    if visibility == "signed":
        # Let Evergreen resolve project credentials for download signing; never write secrets.
        artifact.update(aws_key_var_name="aws_key_new", aws_secret_var_name="aws_secret")
    manifest.write_text(json.dumps([artifact]) + "\n", encoding="utf-8")
    if preserved:
        LOGGER.info(
            "Preserved existing release artifact s3://%s/%s; rebuilt bytes were not uploaded. "
            "Replacing it requires manual deletion in S3.",
            BUCKET,
            key,
        )
    else:
        LOGGER.info("Uploaded %s to s3://%s/%s", local_file, BUCKET, key)


def _restore_release_inputs(
    client: Any,
    identity: TaskIdentity,
    archive: Path,
    relative_name: str,
    inputs: Mapping[str, Path],
) -> str:
    """Publish one immutable checkpoint before exposing any binary or provenance sidecars.

    A task can fail between any two public uploads. The checkpoint retains the original archive,
    provenance and execution log together, so a restart can finish all uploads from the same build.
    Return its ETag for linking the archive to the winning checkpoint, including creation races.
    """
    files = {"archive": archive, **inputs}
    if "archive" in inputs:
        raise ValueError("release input name 'archive' is reserved")
    for name in inputs:
        _validate_component(name)
    if len({os.path.abspath(path) for path in files.values()}) != len(files):
        raise ValueError("Release input paths must be distinct")
    required = {"archive"} | (REQUIRED_RELEASE_INPUTS & inputs.keys())
    checkpoint_name = f"{relative_name}.release-inputs.tar"
    key = identity.key(checkpoint_name)
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        checkpoint = root / "checkpoint.tar"
        existing = _existing_release_artifact(client, identity, key, allow_conditional_create=True)
        if existing is None:
            if (
                _existing_release_artifact(
                    client, identity, identity.key(relative_name), allow_conditional_create=True
                )
                is not None
            ):
                raise ValueError(
                    "Existing release archive has no recovery checkpoint; manual recovery is required"
                )
            if not archive.exists():
                raise FileNotFoundError(archive)
            for name in required:
                if not files[name].is_file():
                    raise ValueError(f"Missing or non-regular required release input: {name}")
            total_size = sum(path.stat().st_size for path in files.values() if path.is_file())
            _check_checkpoint_space(root, total_size * 2)
            if total_size > MAX_CHECKPOINT_SIZE:
                raise ValueError("Release input checkpoint exceeds the size limit")
            with tarfile.open(checkpoint, "w", dereference=True) as bundle:
                for name, path in files.items():
                    if name == "archive" or path.is_file():
                        bundle.add(path, arcname=name, recursive=False)
            if checkpoint.stat().st_size > MAX_CHECKPOINT_SIZE:
                raise ValueError("Release input checkpoint exceeds the size limit")
            upload_artifact(
                client,
                identity,
                checkpoint,
                checkpoint_name,
                root / "manifest",
                preserve_existing=True,
                content_type="application/tar",
            )
            existing = _existing_release_artifact(client, identity, key)
        if existing is None:
            raise ValueError("Release input checkpoint is not readable")
        checkpoint_etag = existing["ETag"]
        # An ambiguous archive HEAD may have allowed a new private checkpoint above. Never
        # relabel an old archive with it: verify the link before replacing local inputs, and
        # again after a conditional archive creation loses a race. Unreadable objects fail
        # the required read after 412; no public archive or sidecar is attached on failure.
        _verify_release_checkpoint(
            _existing_release_artifact(
                client, identity, identity.key(relative_name), allow_conditional_create=True
            ),
            checkpoint_etag,
        )
        size = existing["ContentLength"]
        if size > MAX_CHECKPOINT_SIZE:
            raise ValueError("Release input checkpoint exceeds the size limit")
        _check_checkpoint_space(root, size * 2)
        response = client.get_object(Bucket=BUCKET, Key=key, IfMatch=existing["ETag"])
        with response["Body"] as body, checkpoint.open("wb") as destination:
            _copy_checkpoint_bytes(body, destination, size)
        # Checkpoints are plain tar files. Never accept compression from a bucket writer.
        with tarfile.open(checkpoint, "r:") as bundle:
            members = []
            for member in bundle:
                if len(members) >= len(files):
                    raise ValueError("Release input checkpoint has too many files")
                members.append(member)
            names = {m.name for m in members}
            if not required <= names or not names <= files.keys() or len(names) != len(members):
                raise ValueError("Release input checkpoint has unexpected files")
            total_size = 0
            for member in members:
                if not member.isfile():
                    raise ValueError("Release input checkpoint must contain regular files")
                if member.size < 0 or member.offset_data + member.size > size:
                    raise ValueError("Release input checkpoint has invalid file sizes")
                total_size += member.size
            if total_size > MAX_CHECKPOINT_SIZE:
                raise ValueError("Release input checkpoint exceeds the size limit")
            # Stage every member before deleting or replacing local release inputs. A malformed
            # or truncated checkpoint must leave the current build intact.
            pending_files = {}
            try:
                for member in members:
                    path = files[member.name]
                    _check_checkpoint_space(path.parent, member.size)
                    source = bundle.extractfile(member)
                    assert source is not None
                    with (
                        source,
                        tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as output,
                    ):
                        pending_files[member.name] = Path(output.name)
                        _copy_checkpoint_bytes(source, output, member.size)
                for name, path in files.items():
                    if name not in names:
                        path.unlink(missing_ok=True)
                for name, pending in pending_files.items():
                    # Replace symlinks rather than modifying targets in the Bazel output tree.
                    _replace_release_input(pending, files[name])
            finally:
                for pending in pending_files.values():
                    pending.unlink(missing_ok=True)
        LOGGER.info("Restored release archive and provenance inputs from s3://%s/%s", BUCKET, key)
        return checkpoint_etag


def _replace_release_input(pending: Path, destination: Path) -> None:
    """Replace a staged input, including read-only Bazel outputs on Windows."""
    try:
        pending.replace(destination)
    except PermissionError:
        if os.name != "nt":
            raise
        mode = destination.lstat().st_mode
        # Do not follow symlinks or reinterpret other access failures as read-only files.
        if not stat.S_ISREG(mode) or mode & stat.S_IWRITE:
            raise
        destination.chmod(mode | stat.S_IWRITE)
        try:
            pending.replace(destination)
        finally:
            # Retain the original permissions on both successful and failed replacements.
            destination.chmod(mode)


def _check_checkpoint_space(path: Path, size: int) -> None:
    if shutil.disk_usage(path).free < size + PART_SIZE:
        raise ValueError("Insufficient free space for release input checkpoint")


def _copy_checkpoint_bytes(source: Any, destination: Any, size: int) -> None:
    copied = 0
    while data := source.read(min(1024 * 1024, size - copied + 1)):
        copied += len(data)
        if copied > size:
            raise ValueError("Release input checkpoint exceeds its declared size")
        destination.write(data)
    if copied != size:
        raise ValueError("Release input checkpoint is truncated")


def _parse_release_inputs(
    values: list[str], related_files: str = "[]", root: Path = Path.cwd()
) -> dict[str, Path]:
    inputs = {}
    related = json.loads(related_files)
    if not isinstance(related, list) or any(
        not isinstance(path, str) or not path for path in related
    ):
        raise ValueError("Release related files must be a JSON array of nonempty paths")
    values = [*values, *(f"related-{index}={path}" for index, path in enumerate(related))]
    for value in values:
        name, path = value.split("=", 1)
        _validate_component(name)
        if name == "archive" or name in inputs:
            raise ValueError(f"Duplicate or reserved release input name: {name}")
        if not path:
            raise ValueError(f"Missing release input path: {name}")
        inputs[name] = root / path
    return inputs


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--local-file", type=Path, required=True)
    parser.add_argument("--relative-name", required=True)
    parser.add_argument("--display-name", default="")
    parser.add_argument(
        "--content-type", choices=sorted(CONTENT_TYPES), default="application/octet-stream"
    )
    parser.add_argument("--permissions", default="private")
    parser.add_argument("--visibility", default="signed")
    parser.add_argument("--optional", choices=["true", "false"], default="false")
    parser.add_argument("--preserve-existing", choices=["true", "false", "auto"], default="auto")
    parser.add_argument("--release-input", action="append", default=[], metavar="NAME=PATH")
    parser.add_argument(
        "--release-related-files", default="[]", help="JSON array of optional paths"
    )
    parser.add_argument("--release-input-root", type=Path, default=Path.cwd())
    parser.add_argument("--checkpoint-release-inputs", action="store_true")
    parser.add_argument("--manifest", type=Path, default=Path("task-artifact.json"))
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(message)s")
    try:
        release_inputs = _parse_release_inputs(
            args.release_input, args.release_related_files, args.release_input_root
        )
        identity = TaskIdentity(
            *(
                os.environ[name]
                for name in (
                    "project",
                    "build_variant",
                    "version_id",
                    "task_name",
                    "task_id",
                    "execution",
                )
            )
        )
        upload_artifact(
            get_s3_client(),
            identity,
            args.local_file,
            args.relative_name,
            args.manifest,
            display_name=args.display_name,
            content_type=args.content_type,
            permissions=args.permissions,
            visibility=args.visibility,
            optional=args.optional == "true",
            preserve_existing=(
                is_release_build(os.environ)
                if args.preserve_existing == "auto"
                else args.preserve_existing == "true"
            ),
            release_inputs=(release_inputs if args.checkpoint_release_inputs else None),
        )
    except ClientError as error:
        code = error.response.get("Error", {}).get("Code", "")
        if code in {"PreconditionFailed", "412", "ConditionalRequestConflict", "409"}:
            LOGGER.error("Artifact destination changed during upload; conditional write refused")
        else:
            LOGGER.error("S3 artifact upload failed during %s: %s", error.operation_name, code)
        return 1
    except (KeyError, ValueError, OSError, tarfile.TarError) as error:
        LOGGER.error("Artifact upload failed: %s", error)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
