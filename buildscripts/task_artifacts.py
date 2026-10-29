#!/usr/bin/env python3
"""Upload internal build artifacts to a task-owned prefix with conditional writes."""

import argparse
import json
import logging
import os
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any
from urllib.parse import quote

from botocore.exceptions import ClientError

from buildscripts.util.download_utils import get_s3_client

BUCKET = "mciuploads"
PART_SIZE = 64 * 1024 * 1024
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


def _write_condition(client: Any, identity: TaskIdentity, key: str) -> dict[str, str]:
    """Allow atomic creation, or replacement after verifying ownership and the observed ETag."""
    try:
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
) -> None:
    """Conditionally upload a file and write a credential-free Evergreen artifact attachment.

    Optional only permits a missing local file. A task may replace its own artifacts when retrying
    or restarting. Objects owned by another task, observed newer executions, permission errors,
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
        condition = _write_condition(client, identity, key)
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
                    LOGGER.exception("Could not abort incomplete upload of s3://%s/%s", BUCKET, key)
                raise

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
    LOGGER.info("Uploaded %s to s3://%s/%s", local_file, BUCKET, key)


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
    parser.add_argument("--manifest", type=Path, default=Path("task-artifact.json"))
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(message)s")
    try:
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
        )
    except ClientError as error:
        code = error.response.get("Error", {}).get("Code", "")
        if code in {"PreconditionFailed", "412", "ConditionalRequestConflict", "409"}:
            LOGGER.error("Artifact destination changed during upload; conditional write refused")
        else:
            LOGGER.error("S3 artifact upload failed during %s: %s", error.operation_name, code)
        return 1
    except (KeyError, ValueError, OSError) as error:
        LOGGER.error("Artifact upload failed: %s", error)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
