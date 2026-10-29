"""Activate result tasks as their remote test executions complete.

This watcher runs on the Evergreen host concurrently with `bazel test`. It tails
the Build Event Protocol NDJSON file (--build_event_json_file) and, as each
target's `testSummary` event arrives:
  1. uploads a *filtered* snapshot of the BEP (only the started/testResult/
     testSummary lines, which is all any downstream consumer reads) to S3.
  2. activates the target's result task via the Evergreen API.
"""

import json
import os
import sys
import time
from io import BytesIO
from typing import Annotated, Callable, Optional

import structlog
import typer

if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from buildscripts.evergreen_activate_result_tasks import STREAMED_TASKS_FILE, get_result_tasks
from buildscripts.util.evergreen_retry import get_extra_retry_evergreen_api
from buildscripts.util.fileops import read_yaml_file

LOGGER = structlog.getLogger(__name__)
EVG_CONFIG_FILE = "./.evergreen.yml"
S3_BUCKET = "mciuploads"

app = typer.Typer(pretty_exceptions_show_locals=False)


def parse_test_summary_label(event: dict) -> Optional[str]:
    """Return the target label of a BEP testSummary event, else None.

    testSummary is emitted once per test target when all of its runs/shards
    have completed — the definitive per-target "done" signal.
    """
    if "testSummary" not in event:
        return None
    return event.get("id", {}).get("testSummary", {}).get("label")


def is_filtered_event(event: dict) -> bool:
    """True for the BEP events downstream consumers read from build_events.json."""
    return any(kind in event for kind in ("started", "testResult", "testSummary"))


class BEPTail:
    """Incrementally read complete NDJSON lines from a growing BEP file."""

    def __init__(self, path: str):
        self._path = path
        self._offset = 0

    def read_new_lines(self) -> list[str]:
        if not os.path.exists(self._path):
            return []
        lines = []
        with open(self._path, "rb") as f:
            f.seek(self._offset)
            for raw in f:
                if not raw.endswith(b"\n"):
                    break  # Partial trailing line; re-read it next poll.
                self._offset += len(raw)
                line = raw.decode("utf-8").strip()
                if line:
                    lines.append(line)
        return lines


class ActivationRouter:
    """Routes newly-completed targets to snapshot upload + task activation."""

    def __init__(
        self,
        known_labels: set[str],
        upload_snapshot: Callable[[list[str]], None],
        activate: Callable[[set[str]], set[str]],
    ):
        self._known_labels = known_labels
        self._upload_snapshot = upload_snapshot
        self._activate = activate
        # All filtered (testResult/testSummary) lines seen so far, in order.
        self._filtered_lines: list[str] = []
        # Labels with a testSummary whose result task is not yet activated.
        self._pending: set[str] = set()
        self._activated: set[str] = set()
        # Labels with a testSummary but no known result task (logged once).
        self._unknown: set[str] = set()
        # Whether _filtered_lines has grown since the last successful upload.
        self._snapshot_dirty = False

    def ingest(self, lines: list[str]) -> None:
        """Process new BEP lines: collect filtered lines and completed labels."""
        for line in lines:
            try:
                event = json.loads(line)
            except json.JSONDecodeError:
                LOGGER.warning("Skipping unparseable BEP line", line=line[:200])
                continue
            if not is_filtered_event(event):
                continue
            self._filtered_lines.append(line)
            self._snapshot_dirty = True
            label = parse_test_summary_label(event)
            if label is None or label in self._activated or label in self._unknown:
                continue
            if label not in self._known_labels:
                # No generated result task for this label (e.g. a label the
                # end-of-run assert_all_tests_have_tasks will flag). Not
                # retried; the fallback activation owns this case.
                LOGGER.warning("No result task for completed target", label=label)
                self._unknown.add(label)
                continue
            self._pending.add(label)

    def flush(self) -> None:
        """Upload the snapshot and activate pending tasks.

        A failure leaves the action pending so the next poll retries;
        the end-of-run activation covers anything that never succeeds here.
        """
        if not self._pending:
            return
        if self._snapshot_dirty:
            try:
                self._upload_snapshot(self._filtered_lines)
                self._snapshot_dirty = False
            except Exception:
                LOGGER.warning("Failed to upload build events snapshot; will retry", exc_info=True)
                return  # Never activate a task before its events are uploaded.
        try:
            succeeded = self._activate(set(self._pending))
        except Exception:
            LOGGER.warning("Failed to activate result tasks; will retry", exc_info=True)
            return
        self._activated |= succeeded
        self._pending -= succeeded

    @property
    def activated(self) -> set[str]:
        return set(self._activated)

    @property
    def pending(self) -> set[str]:
        return set(self._pending)


def _pid_alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    return True


def watch(
    tail: BEPTail,
    router: ActivationRouter,
    bazel_pid: int,
    poll_seconds: float,
    sleep: Callable[[float], None] = time.sleep,
) -> None:
    """Poll the BEP until the bazel process exits, then do one final pass."""
    while True:
        done = not _pid_alive(bazel_pid)
        router.ingest(tail.read_new_lines())
        router.flush()
        if done:
            break
        sleep(poll_seconds)
    LOGGER.info(
        "Bazel invocation finished; streaming activation done",
        activated=len(router.activated),
        still_pending=len(router.pending),
    )


def make_snapshot_uploader(bucket: str, key: str) -> Callable[[list[str]], None]:
    """Upload the filtered BEP lines to the key the result tasks fetch."""
    import boto3

    s3 = boto3.client("s3")

    def upload(lines: list[str]) -> None:
        body = ("\n".join(lines) + "\n").encode("utf-8") if lines else b""
        s3.upload_fileobj(BytesIO(body), bucket, key)

    return upload


def publish_final_snapshot(path: str, bucket: str, key: str) -> None:
    """Upload the complete BEP file to the key the result tasks fetch."""
    import boto3

    boto3.client("s3").upload_file(path, bucket, key)


def make_activator(
    evg_api,
    tasks_by_label: dict,
    version_id: str,
    build_variant: str,
    execution: int,
    record_path: Optional[str] = None,
) -> Callable[[set[str]], set[str]]:
    """Activate (or, on a runner restart, restart) the result tasks for labels.

    Every label handled here is appended to `record_path` so the runner's end-of-run activation
    can skip it. That matters on a runner restart (execution > 0), where both paths restart
    already-activated tasks: without the record, the end-of-run pass would restart a second time
    and discard the run this watcher just started, undoing the streaming entirely.
    """

    def record(labels: set[str]) -> None:
        if not record_path or not labels:
            return
        try:
            with open(record_path, "a") as f:
                f.write("".join(f"{label}\n" for label in sorted(labels)))
        except OSError:
            LOGGER.warning("Failed to record streamed labels", exc_info=True)

    def activate(labels: set[str]) -> set[str]:
        succeeded = set()
        to_activate = []
        for label in sorted(labels):
            task = tasks_by_label[label]
            if task.activated:
                if execution > 0:
                    evg_api.restart_task(task.task_id)
                succeeded.add(label)
            else:
                to_activate.append(label)
        if to_activate:
            evg_api.activate_version_tasks(
                version_id, [{"name": build_variant, "tasks": to_activate}]
            )
            succeeded.update(to_activate)
        record(succeeded)
        return succeeded

    return activate


@app.command()
def main(
    expansion_file: Annotated[
        str, typer.Option(help="Location of expansions file generated by evergreen.")
    ],
    build_events_file: Annotated[
        str, typer.Option(help="Path to the Bazel build events NDJSON file being written.")
    ],
    bazel_pid: Annotated[
        int, typer.Option(help="PID of the bazel test process; the watcher exits when it does.")
    ],
    evergreen_config: Annotated[
        str, typer.Option(help="Location of evergreen configuration file.")
    ] = EVG_CONFIG_FILE,
    poll_seconds: Annotated[
        float,
        typer.Option(help="Seconds between BEP polls."),
    ] = 60.0,
) -> None:
    """Stream result-task activation while the runner's bazel test executes."""
    expansions = read_yaml_file(expansion_file)
    build_variant = expansions.get("build_variant")
    version_id = expansions.get("version_id")
    project = expansions.get("project")
    task_name = expansions.get("task_name")
    execution = int(expansions.get("execution", 0))

    if not all([build_variant, version_id, project, task_name]):
        LOGGER.error(
            "Missing required expansions; streaming activation disabled",
            build_variant=build_variant,
            version_id=version_id,
            project=project,
            task_name=task_name,
        )
        return

    evg_api = get_extra_retry_evergreen_api(evergreen_config)
    version = evg_api.version_by_id(version_id)
    build_id = version.build_variants_map.get(build_variant)
    if not build_id:
        LOGGER.error(
            "Build variant not found in version; streaming activation disabled",
            build_variant=build_variant,
            version_id=version_id,
        )
        return

    tasks_by_label = {task.display_name: task for task in get_result_tasks(evg_api, build_id)}
    LOGGER.info("Streaming result task activation", known_tasks=len(tasks_by_label))

    # The key each result task s3.gets in its setup_task (see generate_result_tasks
    # _make_build_events_fetch). The teardown of the runner's task group later overwrites it with
    # the full, canonical BEP.
    key = f"{project}/{version_id}/{build_variant}/{task_name}/build_events.json"

    router = ActivationRouter(
        known_labels=set(tasks_by_label),
        upload_snapshot=make_snapshot_uploader(S3_BUCKET, key),
        activate=make_activator(
            evg_api,
            tasks_by_label,
            version_id,
            build_variant,
            execution,
            record_path=STREAMED_TASKS_FILE,
        ),
    )
    watch(BEPTail(build_events_file), router, bazel_pid, poll_seconds)

    try:
        publish_final_snapshot(build_events_file, S3_BUCKET, key)
    except Exception:
        LOGGER.warning("Failed to publish the final build events snapshot", exc_info=True)


if __name__ == "__main__":
    app()
