"""Best-effort, detached OpenTelemetry reporting for quality checks."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import time
import traceback
from collections.abc import Iterable, Mapping
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Self

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from buildscripts.quality_checks.models import CheckResult, CheckStatus

DEFAULT_SERVICE_NAME = "mongo-quality-checks"
DEFAULT_OTEL_COLLECTOR_ENDPOINT = "otel-collector.prod.corp.mongodb.com:443"
MAX_PATH_SAMPLES = 100
_UPLOAD_FLAG = "--_upload-telemetry-payload"


@dataclass(frozen=True, slots=True)
class RepoPathSample:
    total_count: int
    paths: tuple[str, ...]
    truncated: bool


def sample_repo_relative_paths(
    repo_root: Path, paths: Iterable[str | os.PathLike[str]]
) -> RepoPathSample:
    root = os.path.abspath(os.fspath(repo_root))
    normalized: list[str] = []
    seen: set[str] = set()
    for raw_path in paths:
        try:
            raw = os.fspath(raw_path)
            if "\0" in raw:
                continue
            candidate = os.path.abspath(raw if os.path.isabs(raw) else os.path.join(root, raw))
            if os.path.commonpath((root, candidate)) != root:
                continue
            relative = Path(os.path.relpath(candidate, root)).as_posix()
        except (OSError, TypeError, ValueError):
            continue
        if relative in {"", "."}:
            continue
        if relative not in seen:
            seen.add(relative)
            normalized.append(relative)
    return RepoPathSample(
        len(normalized), tuple(normalized[:MAX_PATH_SAMPLES]), len(normalized) > MAX_PATH_SAMPLES
    )


def _run_git_for_telemetry(repo_root: Path, *args: str) -> str | None:
    try:
        result = subprocess.run(
            ["git", *args],
            cwd=repo_root,
            check=True,
            capture_output=True,
            text=True,
            timeout=2,
        )
        return result.stdout.strip() or None
    except (OSError, subprocess.SubprocessError):
        return None


def _truthy(value: str | None) -> bool:
    return bool(value and value.lower() not in {"0", "false", "no", "off"})


def build_origin_attributes(repo_root: Path) -> dict[str, object]:
    is_ci = _truthy(os.environ.get("CI"))
    sha = (
        os.environ.get("EVG_REVISION")
        or os.environ.get("GITHUB_SHA")
        or _run_git_for_telemetry(repo_root, "rev-parse", "HEAD")
    )
    branch = (
        os.environ.get("branch_name")
        or os.environ.get("GITHUB_HEAD_REF")
        or os.environ.get("GITHUB_REF_NAME")
        or os.environ.get("BRANCH_NAME")
        or _run_git_for_telemetry(repo_root, "branch", "--show-current")
    )
    attributes: dict[str, object] = {
        "mongo.quality_checks.origin": "ci" if is_ci else "local",
        "mongo.quality_checks.is_ci": is_ci,
    }
    if sha:
        attributes["mongo.quality_checks.git_sha"] = sha
    if branch:
        attributes["mongo.quality_checks.git_branch"] = branch
    for env_name, attribute in (
        ("task_id", "evergreen_task_id"),
        ("build_variant", "evergreen_build_variant"),
        ("execution", "evergreen_execution"),
    ):
        if value := os.environ.get(env_name):
            attributes[f"mongo.quality_checks.{attribute}"] = value
    return attributes


class TelemetryClient:
    """Collect plain span records and export them after the command exits."""

    def __init__(self, repo_root: Path) -> None:
        self.repo_root = repo_root.resolve()
        self.started_time_ns = time.time_ns()
        self.invocation_attributes: dict[str, object] = build_origin_attributes(self.repo_root)
        self.check_records: list[dict[str, Any]] = []

    @classmethod
    def create(cls, repo_root: Path) -> Self:
        return cls(repo_root)

    def set_invocation_attributes(self, attributes: Mapping[str, object]) -> None:
        self.invocation_attributes.update(attributes)

    def record_candidate_files(
        self,
        paths: Iterable[str],
        *,
        trigger_mode: str,
        candidate_count: int | None = None,
    ) -> None:
        sample = sample_repo_relative_paths(self.repo_root, paths)
        self.invocation_attributes.update(
            {
                "mongo.quality_checks.trigger_mode": trigger_mode,
                "mongo.quality_checks.candidate_count": (
                    sample.total_count if candidate_count is None else candidate_count
                ),
                "mongo.quality_checks.candidate_path_count": sample.total_count,
                "mongo.quality_checks.candidate_paths": list(sample.paths),
                "mongo.quality_checks.candidate_paths_truncated": sample.truncated,
            }
        )

    def record_result(self, result: CheckResult) -> None:
        sample = sample_repo_relative_paths(self.repo_root, result.matched_files)
        attributes: dict[str, object] = {
            **self.invocation_attributes,
            "mongo.quality_checks.check_id": result.check_id,
            "mongo.quality_checks.check_name": result.display_name,
            "mongo.quality_checks.group": result.spec.group,
            "mongo.quality_checks.phase": result.phase.value,
            "mongo.quality_checks.operation": result.operation,
            "mongo.quality_checks.status": result.status.value,
            "mongo.quality_checks.duration_seconds": result.duration_seconds,
            "mongo.quality_checks.matched_file_count": result.matched_file_count
            if result.matched_file_count is not None
            else sample.total_count,
            "mongo.quality_checks.matched_paths": list(sample.paths),
            "mongo.quality_checks.matched_paths_truncated": sample.truncated,
        }
        if result.exit_code is not None:
            attributes["mongo.quality_checks.exit_code"] = result.exit_code
        if sample.total_count:
            attributes["mongo.quality_checks.seconds_per_file"] = (
                result.duration_seconds / sample.total_count
            )
        self.check_records.append(
            {
                "name": f"mongo.quality_checks.{result.check_id}",
                "start_time_ns": result.finished_time_ns
                - int(result.duration_seconds * 1_000_000_000),
                "end_time_ns": result.finished_time_ns,
                "attributes": attributes,
                "error": result.status == CheckStatus.FAIL,
            }
        )

    def record_preparation(
        self,
        *,
        duration_seconds: float,
        status: str = "PASS",
        detail: str | None = None,
    ) -> None:
        """Record candidate discovery, manifest validation, and tool preparation."""

        finished_time_ns = time.time_ns()
        attributes: dict[str, object] = {
            **self.invocation_attributes,
            "mongo.quality_checks.phase": "preparation",
            "mongo.quality_checks.status": status,
            "mongo.quality_checks.duration_seconds": duration_seconds,
        }
        if detail:
            # Keep telemetry diagnostic-only: callers must not put tool output or
            # file contents here. Exception class/message is sufficient.
            attributes["mongo.quality_checks.detail"] = detail[:500]
        self.check_records.append(
            {
                "name": "mongo.quality_checks.preparation",
                "start_time_ns": finished_time_ns - int(duration_seconds * 1_000_000_000),
                "end_time_ns": finished_time_ns,
                "attributes": attributes,
                "error": status != "PASS",
            }
        )

    def finish(
        self,
        *,
        completed_checks: int,
        failed_checks: int,
        serial_checks: int,
        parallel_checks: int,
        wallclock_seconds: float,
        phase_timings: Mapping[str, float] | None = None,
        verbose: bool = False,
    ) -> Path | None:
        endpoint = (
            os.environ.get("OTEL_EXPORTER_OTLP_TRACES_ENDPOINT")
            or os.environ.get("OTEL_EXPORTER_OTLP_ENDPOINT")
            or DEFAULT_OTEL_COLLECTOR_ENDPOINT
        )
        if not endpoint or _truthy(os.environ.get("OTEL_SDK_DISABLED")):
            return None
        self.invocation_attributes.update(
            {
                "mongo.quality_checks.completed_checks": completed_checks,
                "mongo.quality_checks.failed_checks": failed_checks,
                "mongo.quality_checks.serial_checks": serial_checks,
                "mongo.quality_checks.parallel_checks": parallel_checks,
                "mongo.quality_checks.wallclock_seconds": wallclock_seconds,
                "mongo.quality_checks.status": "FAIL" if failed_checks else "PASS",
            }
        )
        for phase, elapsed in (phase_timings or {}).items():
            self.invocation_attributes[f"mongo.quality_checks.phase.{phase}_seconds"] = elapsed
        payload = {
            "service_name": os.environ.get("OTEL_SERVICE_NAME", DEFAULT_SERVICE_NAME),
            "endpoint": endpoint,
            "timeout_seconds": _export_timeout_seconds(),
            "root": {
                "name": "mongo.quality_checks.invocation",
                "start_time_ns": self.started_time_ns,
                "end_time_ns": time.time_ns(),
                "attributes": self.invocation_attributes,
                "error": failed_checks > 0,
            },
            "children": self.check_records,
        }
        payload_path: Path | None = None
        log_path: Path | None = None
        stdout: int | Any = subprocess.DEVNULL
        try:
            fd, raw_path = tempfile.mkstemp(
                prefix="mongo-quality-checks-telemetry-", suffix=".json"
            )
            payload_path = Path(raw_path)
            with os.fdopen(fd, "w", encoding="utf-8") as payload_file:
                json.dump(payload, payload_file, separators=(",", ":"))
            if verbose:
                log_fd, raw_log = tempfile.mkstemp(
                    prefix="mongo-quality-checks-telemetry-", suffix=".log"
                )
                os.close(log_fd)
                log_path = Path(raw_log)
                stdout = log_path.open("wb")
            detach_options: dict[str, Any]
            if os.name == "nt":
                detach_options = {
                    "creationflags": subprocess.DETACHED_PROCESS
                    | subprocess.CREATE_NEW_PROCESS_GROUP
                    | subprocess.CREATE_NO_WINDOW
                }
            else:
                detach_options = {"start_new_session": True}
            subprocess.Popen(
                [sys.executable, str(Path(__file__).resolve()), _UPLOAD_FLAG, str(payload_path)],
                stdin=subprocess.DEVNULL,
                stdout=stdout,
                stderr=subprocess.STDOUT,
                close_fds=True,
                **detach_options,
            )
            if hasattr(stdout, "close"):
                stdout.close()
            return log_path
        except Exception as exc:
            if hasattr(stdout, "close"):
                stdout.close()
            if payload_path is not None:
                payload_path.unlink(missing_ok=True)
            if log_path is not None:
                try:
                    with log_path.open("a", encoding="utf-8") as log_file:
                        _report_telemetry_failure("launch", exc, stream=log_file)
                except OSError:
                    pass
                return log_path
            return None


def _export_timeout_seconds() -> int:
    try:
        timeout = os.environ.get("OTEL_EXPORTER_OTLP_TRACES_TIMEOUT") or os.environ.get(
            "OTEL_EXPORTER_OTLP_TIMEOUT", "30"
        )
        return max(1, int(float(timeout)))
    except ValueError:
        return 30


def _report_telemetry_failure(stage: str, exc: Exception, *, stream: Any) -> None:
    """Write exporter diagnostics to its redirected stream without affecting callers."""

    print(f"Telemetry exporter {stage} failed: {type(exc).__name__}: {exc}", file=stream)
    traceback.print_exception(type(exc), exc, exc.__traceback__, file=stream)


def _run_telemetry_uploader(payload_path: Path) -> int:
    try:
        payload = json.loads(payload_path.read_text(encoding="utf-8"))
    except Exception as exc:
        _report_telemetry_failure("payload read", exc, stream=sys.stderr)
        return 0
    finally:
        try:
            payload_path.unlink(missing_ok=True)
        except OSError as exc:
            _report_telemetry_failure("payload cleanup", exc, stream=sys.stderr)
    try:
        from opentelemetry import trace
        from opentelemetry.exporter.otlp.proto.grpc.trace_exporter import OTLPSpanExporter
        from opentelemetry.sdk.resources import SERVICE_NAME, Resource
        from opentelemetry.sdk.trace import TracerProvider
        from opentelemetry.sdk.trace.export import BatchSpanProcessor
        from opentelemetry.trace import Status, StatusCode

        provider = TracerProvider(resource=Resource.create({SERVICE_NAME: payload["service_name"]}))
        provider.add_span_processor(
            BatchSpanProcessor(
                OTLPSpanExporter(
                    endpoint=payload["endpoint"], timeout=payload.get("timeout_seconds", 30)
                )
            )
        )
        tracer = provider.get_tracer("mongo-quality-checks")
        root_record = payload["root"]
        root = tracer.start_span(
            root_record["name"],
            start_time=root_record["start_time_ns"],
            attributes=root_record["attributes"],
        )
        if root_record.get("error"):
            root.set_status(Status(StatusCode.ERROR))
        root_context = trace.set_span_in_context(root)
        for record in payload.get("children", []):
            child = tracer.start_span(
                record["name"],
                context=root_context,
                start_time=record["start_time_ns"],
                attributes=record["attributes"],
            )
            if record.get("error"):
                child.set_status(Status(StatusCode.ERROR))
            child.end(end_time=record["end_time_ns"])
        root.end(end_time=root_record["end_time_ns"])
        provider.shutdown()
    except Exception as exc:
        _report_telemetry_failure("upload", exc, stream=sys.stderr)
        return 0
    return 0


def main(argv: list[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    if len(args) == 2 and args[0] == _UPLOAD_FLAG:
        return _run_telemetry_uploader(Path(args[1]))
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
