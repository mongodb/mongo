"""Helpers for validating package build provenance."""

import argparse
import collections
import contextlib
import dataclasses
import hashlib
import json
import os
import shutil
import subprocess
import tarfile
import zipfile
from pathlib import Path
from typing import Any, BinaryIO, Iterable, Iterator

import requests
from google.protobuf.message import DecodeError

from buildscripts import bazel_spawn_pb2

RELEASE_BINARY_PROVENANCE_ARTIFACT_NAME = "Release Binary Provenance"
RELEASE_EXECUTION_LOG_ARTIFACT_NAME = "Release Execution Log"
RELEASE_BINARY_NAMES = ("mongod", "mongos")
CRYPT_RELEASE_BINARY_NAMES = ("mongo_crypt_v1.so",)
DISALLOWED_EXECUTION_LOG_RUNNERS = {"remote", "remote cache hit"}
SERVER_RELEASE_PROJECT_PREFIX = "mongo-release"

RELEASE_LOCAL_SAFETY_FLAGS = (
    "--remote_executor= --noremote_accept_cached "
    "--remote_upload_local_results=false --modify_execution_info=.*=+no-cache"
)


def is_server_release_project(evg_project: str | None) -> bool:
    """Return whether this Evergreen project is a server release project."""

    return bool(evg_project) and evg_project.startswith(SERVER_RELEASE_PROJECT_PREFIX)


def find_build_task(tasks: list[Any], task_display_name: str) -> Any:
    """Find a unique Evergreen task by display name."""

    matching_tasks = [task for task in tasks if task.display_name == task_display_name]

    if not matching_tasks:
        raise RuntimeError(f"Could not find task '{task_display_name}' in build task list")

    if len(matching_tasks) > 1:
        matching_task_ids = ", ".join(task.task_id for task in matching_tasks)
        raise RuntimeError(
            f"Found multiple tasks named '{task_display_name}' in build task list: "
            f"{matching_task_ids}"
        )

    return matching_tasks[0]


def find_task_artifact_url(task: Any, artifact_name: str) -> str:
    """Find a unique artifact URL on a task."""

    matching_artifacts = [artifact for artifact in task.artifacts if artifact.name == artifact_name]

    if not matching_artifacts:
        raise RuntimeError(
            f"Could not find '{artifact_name}' artifact for task {task.task_id} "
            f"({task.display_name})"
        )

    if len(matching_artifacts) > 1:
        matching_urls = ", ".join(artifact.url for artifact in matching_artifacts)
        raise RuntimeError(
            f"Found multiple '{artifact_name}' artifacts for task {task.task_id}: {matching_urls}"
        )

    return matching_artifacts[0].url


def fetch_task_artifact_text(task: Any, artifact_name: str) -> str:
    """Fetch a text artifact from an Evergreen task."""

    artifact_url = find_task_artifact_url(task, artifact_name)
    response = requests.get(artifact_url, timeout=300)
    response.raise_for_status()
    return response.text


def fetch_task_artifact_to_file(task: Any, artifact_name: str, output_path: Path) -> Path:
    """Fetch a binary artifact from an Evergreen task."""

    artifact_url = find_task_artifact_url(task, artifact_name)
    response = requests.get(artifact_url, stream=True, timeout=300)
    response.raise_for_status()

    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("wb") as file_handle:
        for chunk in response.iter_content(chunk_size=1024 * 1024):
            if chunk:
                file_handle.write(chunk)

    return output_path


def fetch_release_binary_provenance_from_tasks(tasks: list[Any], task_display_name: str) -> str:
    """Fetch release binary provenance from an Evergreen task list."""

    build_task = find_build_task(tasks, task_display_name)
    return fetch_task_artifact_text(build_task, RELEASE_BINARY_PROVENANCE_ARTIFACT_NAME)


def fetch_release_execution_log_from_tasks(
    tasks: list[Any], task_display_name: str, output_path: Path
) -> Path:
    """Fetch release execution log from an Evergreen task list."""

    build_task = find_build_task(tasks, task_display_name)
    return fetch_task_artifact_to_file(build_task, RELEASE_EXECUTION_LOG_ARTIFACT_NAME, output_path)


def validate_release_build_command(build_command: str) -> None:
    """Validate that a captured Bazel build command ends with the release-local safety suffix."""

    build_command = build_command.strip()
    if not build_command:
        raise ValueError("Bazel build command is empty")

    required_suffix = f" {RELEASE_LOCAL_SAFETY_FLAGS}"
    if not build_command.endswith(required_suffix):
        raise ValueError(
            "Bazel build command does not end with the required release-local safety suffix: "
            f"{RELEASE_LOCAL_SAFETY_FLAGS}"
        )


def hash_file(path: Path) -> str:
    """Return the SHA256 digest for a file."""

    digest = hashlib.sha256()
    with path.open("rb") as file_handle:
        for chunk in iter(lambda: file_handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def release_binary_name_from_path(
    path: str, binary_names: Iterable[str] = RELEASE_BINARY_NAMES
) -> str | None:
    """Return the canonical release binary name for a path, accepting Windows .exe suffixes."""

    filename = path.replace("\\", "/").rsplit("/", 1)[-1].lower()

    for binary_name in binary_names:
        normalized_binary_name = binary_name.lower()
        if filename in {normalized_binary_name, f"{normalized_binary_name}.exe"}:
            return binary_name

    return None


def hash_release_binaries_in_directory(
    binary_dir: Path, binary_names: Iterable[str] = RELEASE_BINARY_NAMES
) -> dict[str, dict[str, str]]:
    """Hash release binaries from a directory, accepting Windows .exe suffixes."""

    hashes = {}
    binary_names = tuple(binary_names)
    for binary_path in binary_dir.iterdir():
        if not binary_path.is_file():
            continue
        binary_name = release_binary_name_from_path(binary_path.name, binary_names)
        if binary_name is None:
            continue
        hashes[binary_name] = {
            "path": str(binary_path),
            "sha256": hash_file(binary_path),
        }

    missing_binaries = sorted(set(binary_names) - set(hashes))
    if missing_binaries:
        raise RuntimeError(
            f"Could not find release binaries in {binary_dir}: {', '.join(missing_binaries)}"
        )
    return hashes


def hash_release_binaries_in_archive(
    archive_path: Path, binary_names: Iterable[str] = RELEASE_BINARY_NAMES
) -> dict[str, dict[str, str]]:
    """Hash release binaries from a tar or zip archive, accepting Windows .exe suffixes."""

    hashes = {}
    binary_names = tuple(binary_names)
    if zipfile.is_zipfile(archive_path):
        with zipfile.ZipFile(archive_path) as archive:
            for member in archive.infolist():
                if member.is_dir():
                    continue
                binary_name = release_binary_name_from_path(member.filename, binary_names)
                if binary_name is None:
                    continue
                digest = hashlib.sha256()
                with archive.open(member) as extracted:
                    for chunk in iter(lambda: extracted.read(1024 * 1024), b""):
                        digest.update(chunk)
                hashes[binary_name] = {
                    "path": member.filename,
                    "sha256": digest.hexdigest(),
                }
        return _ensure_release_binary_hashes_complete(archive_path, hashes, binary_names)

    with tarfile.open(archive_path, "r:*") as tar:
        for member in tar.getmembers():
            if not member.isfile():
                continue
            binary_name = release_binary_name_from_path(member.name, binary_names)
            if binary_name is None:
                continue
            extracted = tar.extractfile(member)
            if extracted is None:
                continue
            digest = hashlib.sha256()
            try:
                for chunk in iter(lambda: extracted.read(1024 * 1024), b""):
                    digest.update(chunk)
            finally:
                extracted.close()
            hashes[binary_name] = {
                "path": member.name,
                "sha256": digest.hexdigest(),
            }

    return _ensure_release_binary_hashes_complete(archive_path, hashes, binary_names)


def _ensure_release_binary_hashes_complete(
    search_path: Path,
    hashes: dict[str, dict[str, str]],
    binary_names: Iterable[str] = RELEASE_BINARY_NAMES,
) -> dict[str, dict[str, str]]:
    """Raise if the release binary hash set is incomplete."""

    missing_binaries = sorted(set(binary_names) - set(hashes))
    if missing_binaries:
        raise RuntimeError(
            f"Could not find release binaries in {search_path}: {', '.join(missing_binaries)}"
        )
    return hashes


def make_release_binary_provenance(
    build_command_file: Path,
    dist_archive: Path,
    binary_names: Iterable[str] = RELEASE_BINARY_NAMES,
) -> dict[str, Any]:
    """Create release binary provenance from a build command and release archive."""

    build_command = build_command_file.read_text()
    validate_release_build_command(build_command)
    return {
        "build_id": os.environ.get("build_id", ""),
        "build_variant": os.environ.get("build_variant", ""),
        "execution": os.environ.get("execution", ""),
        "revision": os.environ.get("revision", ""),
        "task_id": os.environ.get("task_id", ""),
        "task_name": os.environ.get("task_name", ""),
        "build_command": build_command,
        "binaries": hash_release_binaries_in_archive(dist_archive, binary_names),
    }


def should_create_release_binary_provenance() -> bool:
    """Return whether this task should create release binary provenance."""

    return release_binary_provenance_skip_reason() is None


def release_binary_provenance_skip_reason() -> str | None:
    """Return the reason to skip release binary provenance, if any."""

    if os.environ.get("task_name") == "package" and not is_server_release_project(
        os.environ.get("project")
    ):
        return (
            "package task in non-server-release project "
            f"(project={os.environ.get('project', '')})"
        )

    if os.environ.get("is_patch") == "true" and os.environ.get("is_release", "false") == "false":
        return (
            "ordinary patch build "
            f"(is_patch={os.environ.get('is_patch')}, "
            f"is_release={os.environ.get('is_release', 'false')})"
        )

    return None


def validate_release_binary_provenance(
    provenance_text: str,
    binary_dir: Path,
    expected_build_id: str | None = None,
    binary_names: Iterable[str] = RELEASE_BINARY_NAMES,
) -> None:
    """Validate command provenance and hashes for extracted release binaries."""

    actual_binaries = hash_release_binaries_in_directory(binary_dir, binary_names)
    validate_release_binary_provenance_hashes(
        provenance_text, actual_binaries, expected_build_id, binary_names
    )


def validate_release_binary_provenance_against_archive(
    provenance_text: str,
    archive_path: Path,
    expected_build_id: str | None = None,
    binary_names: Iterable[str] = RELEASE_BINARY_NAMES,
) -> None:
    """Validate command provenance and hashes for release binaries in an archive."""

    actual_binaries = hash_release_binaries_in_archive(archive_path, binary_names)
    validate_release_binary_provenance_hashes(
        provenance_text, actual_binaries, expected_build_id, binary_names
    )


def validate_release_binary_provenance_hashes(
    provenance_text: str,
    actual_binaries: dict[str, dict[str, str]],
    expected_build_id: str | None = None,
    binary_names: Iterable[str] = RELEASE_BINARY_NAMES,
) -> None:
    """Validate command provenance and release binary hashes."""

    binary_names = tuple(binary_names)
    try:
        provenance = json.loads(provenance_text)
    except json.JSONDecodeError as exc:
        raise ValueError(f"Could not parse release binary provenance JSON: {exc}") from exc

    if expected_build_id and provenance.get("build_id") not in {None, "", expected_build_id}:
        raise ValueError(
            "Release binary provenance build_id does not match package test build_id: "
            f"{provenance.get('build_id')} != {expected_build_id}"
        )

    build_command = provenance.get("build_command")
    if not isinstance(build_command, str):
        raise ValueError("Release binary provenance is missing a string build_command")
    validate_release_build_command(build_command)

    expected_binaries = provenance.get("binaries")
    if not isinstance(expected_binaries, dict):
        raise ValueError("Release binary provenance is missing a binaries object")

    for binary_name in binary_names:
        expected_binary = expected_binaries.get(binary_name)
        if not isinstance(expected_binary, dict):
            raise ValueError(f"Release binary provenance is missing binary: {binary_name}")

        expected_hash = expected_binary.get("sha256")
        actual_hash = actual_binaries[binary_name]["sha256"]
        if expected_hash != actual_hash:
            raise ValueError(
                f"Release binary hash mismatch for {binary_name}: "
                f"expected {expected_hash}, found {actual_hash}"
            )


@dataclasses.dataclass(frozen=True)
class ExecutionLogSpawn:
    """Relevant fields from a compact execution log spawn entry."""

    runner: str
    cache_hit: bool
    target_label: str = ""
    mnemonic: str = ""


@dataclasses.dataclass(frozen=True)
class ExecutionLogValidationSummary:
    """Summary of compact execution log validation."""

    spawn_count: int
    cache_hit_count: int
    runner_counts: dict[str, int]


def validate_compact_execution_log_file(
    execution_log_path: Path, allow_empty: bool = False
) -> ExecutionLogValidationSummary:
    """Validate a zstd-compressed Bazel compact execution log."""

    with open_zstd_stream(execution_log_path) as stream:
        return validate_compact_execution_log_stream(stream, allow_empty=allow_empty)


def validate_compact_execution_log_stream(
    stream: BinaryIO, allow_empty: bool = False
) -> ExecutionLogValidationSummary:
    """Validate a stream of length-delimited compact execution log entries."""

    return validate_compact_execution_log_entries(
        iter_compact_execution_log_entries(stream), allow_empty=allow_empty
    )


def validate_compact_execution_log_bytes(
    data: bytes, allow_empty: bool = False
) -> ExecutionLogValidationSummary:
    """Validate uncompressed length-delimited compact execution log bytes."""

    return validate_compact_execution_log_entries(
        iter_length_delimited_messages(data), allow_empty=allow_empty
    )


def validate_compact_execution_log_entries(
    entries: Iterable[bytes], allow_empty: bool = False
) -> ExecutionLogValidationSummary:
    """Validate compact execution log entries and return aggregate counts."""

    spawn_count = 0
    cache_hit_count = 0
    runner_counts: collections.Counter[str] = collections.Counter()
    violations: list[ExecutionLogSpawn] = []

    for entry in entries:
        spawn = parse_compact_execution_log_spawn(entry)
        if spawn is None:
            continue

        spawn_count += 1
        runner = spawn.runner.strip().lower()
        runner_counts[runner or "<unset>"] += 1
        if spawn.cache_hit:
            cache_hit_count += 1

        if runner in DISALLOWED_EXECUTION_LOG_RUNNERS:
            violations.append(spawn)

    if spawn_count == 0 and not allow_empty:
        raise ValueError(
            "Bazel compact execution log contains no spawn entries. "
            "This cannot prove the release binaries avoided remote execution/cache."
        )

    if violations:
        examples = ", ".join(
            f"runner={spawn.runner!r} mnemonic={spawn.mnemonic!r} target={spawn.target_label!r}"
            for spawn in violations[:5]
        )
        raise ValueError(
            "Bazel compact execution log contains remote execution/cache runner(s): " f"{examples}"
        )

    return ExecutionLogValidationSummary(
        spawn_count=spawn_count,
        cache_hit_count=cache_hit_count,
        runner_counts=dict(sorted(runner_counts.items())),
    )


def combine_execution_log_validation_summaries(
    summaries: Iterable[ExecutionLogValidationSummary],
) -> ExecutionLogValidationSummary:
    """Combine multiple execution log validation summaries."""

    spawn_count = 0
    cache_hit_count = 0
    runner_counts: collections.Counter[str] = collections.Counter()

    for summary in summaries:
        spawn_count += summary.spawn_count
        cache_hit_count += summary.cache_hit_count
        runner_counts.update(summary.runner_counts)

    if spawn_count == 0:
        raise ValueError(
            "Bazel compact execution logs contain no spawn entries. "
            "This cannot prove the release binaries avoided remote execution/cache."
        )

    return ExecutionLogValidationSummary(
        spawn_count=spawn_count,
        cache_hit_count=cache_hit_count,
        runner_counts=dict(sorted(runner_counts.items())),
    )


def parse_compact_execution_log_spawn(entry: bytes) -> ExecutionLogSpawn | None:
    """Parse the Spawn payload from one compact ExecLogEntry message."""

    exec_log_entry = bazel_spawn_pb2.ExecLogEntry()
    try:
        exec_log_entry.ParseFromString(entry)
    except DecodeError as exc:
        raise ValueError("Could not parse compact execution log entry") from exc

    if exec_log_entry.WhichOneof("type") != "spawn":
        return None

    spawn = exec_log_entry.spawn

    return ExecutionLogSpawn(
        runner=spawn.runner,
        cache_hit=spawn.cache_hit,
        target_label=spawn.target_label,
        mnemonic=spawn.mnemonic,
    )


def iter_compact_execution_log_entries(stream: BinaryIO) -> Iterator[bytes]:
    """Yield length-delimited protobuf messages from a binary stream."""

    while True:
        first_byte = stream.read(1)
        if not first_byte:
            return

        size = read_varint_from_stream(stream, first_byte[0])
        yield read_exact(stream, size)


def iter_length_delimited_messages(data: bytes) -> Iterator[bytes]:
    """Yield length-delimited protobuf messages from bytes."""

    position = 0
    while position < len(data):
        size, position = read_varint_from_bytes(data, position)
        end = position + size
        if end > len(data):
            raise ValueError("Truncated length-delimited compact execution log entry")
        yield data[position:end]
        position = end


def read_varint_from_stream(stream: BinaryIO, first_byte: int) -> int:
    """Read a protobuf varint from a stream after the first byte has been consumed."""

    value = first_byte & 0x7F
    shift = 7
    current_byte = first_byte
    while current_byte & 0x80:
        next_byte = stream.read(1)
        if not next_byte:
            raise ValueError("Truncated protobuf varint in compact execution log")
        current_byte = next_byte[0]
        value |= (current_byte & 0x7F) << shift
        shift += 7
        if shift > 63:
            raise ValueError("Invalid protobuf varint in compact execution log")
    return value


def read_varint_from_bytes(data: bytes, position: int) -> tuple[int, int]:
    """Read a protobuf varint from bytes."""

    value = 0
    shift = 0
    while position < len(data):
        current_byte = data[position]
        position += 1
        value |= (current_byte & 0x7F) << shift
        if not current_byte & 0x80:
            return value, position
        shift += 7
        if shift > 63:
            raise ValueError("Invalid protobuf varint in compact execution log")

    raise ValueError("Truncated protobuf varint in compact execution log")


def read_exact(stream: BinaryIO, size: int) -> bytes:
    """Read exactly size bytes from a binary stream."""

    chunks = []
    remaining = size
    while remaining:
        chunk = stream.read(remaining)
        if not chunk:
            raise ValueError("Truncated compact execution log entry")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


@contextlib.contextmanager
def open_zstd_stream(path: Path) -> Iterator[BinaryIO]:
    """Open a zstd-compressed file with pyzstd or the zstd CLI fallback."""

    try:
        from pyzstd import ZstdFile
    except ImportError:
        zstd_binary = shutil.which("zstd")
        if not zstd_binary:
            raise RuntimeError(
                "Validating Bazel compact execution logs requires pyzstd or the zstd binary"
            )

        process = subprocess.Popen(
            [zstd_binary, "-dc", str(path)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if process.stdout is None:
            raise RuntimeError("Failed to open zstd decompression stream")
        validation_raised = False
        try:
            yield process.stdout
        except BaseException:
            validation_raised = True
            raise
        finally:
            process.stdout.close()
            stderr = process.stderr.read().decode("utf-8", errors="replace")
            return_code = process.wait()
            if return_code and not validation_raised:
                raise RuntimeError(
                    f"zstd failed to decompress {path} with exit code {return_code}: {stderr}"
                )
    else:
        with ZstdFile(path, mode="rb") as stream:
            yield stream


def create_provenance_file(
    build_command_file: Path,
    dist_archive: Path,
    output_file: Path,
    execution_log_file: Path | None = None,
    binary_names: Iterable[str] = RELEASE_BINARY_NAMES,
    skip_if_build_command_not_release_safe: bool = False,
) -> None:
    """Write release binary provenance JSON."""

    skip_reason = release_binary_provenance_skip_reason()
    if skip_reason:
        output_file.unlink(missing_ok=True)
        print(f"Skipping release binary provenance creation for {skip_reason}")
        return

    if skip_if_build_command_not_release_safe:
        build_command = build_command_file.read_text()
        try:
            validate_release_build_command(build_command)
        except ValueError as exc:
            output_file.unlink(missing_ok=True)
            print(f"Skipping release binary provenance creation: {exc}")
            return

    if execution_log_file is not None and not execution_log_file.is_file():
        raise RuntimeError(f"Release execution log file does not exist: {execution_log_file}")

    provenance = make_release_binary_provenance(build_command_file, dist_archive, binary_names)
    output_file.write_text(json.dumps(provenance, indent=2, sort_keys=True) + "\n")


def main() -> None:
    parser = argparse.ArgumentParser(description="Create package test provenance artifacts.")
    subparsers = parser.add_subparsers(dest="command", required=True)

    create_parser = subparsers.add_parser("create")
    create_parser.add_argument("--build-command-file", type=Path, required=True)
    create_parser.add_argument("--dist-archive", type=Path, required=True)
    create_parser.add_argument("--execution-log-file", type=Path)
    create_parser.add_argument("--binary-name", action="append", dest="binary_names")
    create_parser.add_argument("--skip-if-build-command-not-release-safe", action="store_true")
    create_parser.add_argument("--output", type=Path, required=True)

    args = parser.parse_args()
    if args.command == "create":
        create_provenance_file(
            args.build_command_file,
            args.dist_archive,
            args.output,
            args.execution_log_file,
            args.binary_names or RELEASE_BINARY_NAMES,
            args.skip_if_build_command_not_release_safe,
        )


if __name__ == "__main__":
    main()
