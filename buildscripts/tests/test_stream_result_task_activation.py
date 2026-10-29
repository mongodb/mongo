"""Unit tests for buildscripts/stream_result_task_activation.py."""

import json
import os
import tempfile
import unittest
from unittest.mock import MagicMock, patch

import structlog

from buildscripts.evergreen_activate_result_tasks import (
    activate_result_task_group,
    get_streamed_labels,
)
from buildscripts.stream_result_task_activation import (
    ActivationRouter,
    BEPTail,
    is_filtered_event,
    make_activator,
    parse_test_summary_label,
    watch,
)


def setUpModule():
    """Render structlog tracebacks as plain text.

    Several tests exercise the retry paths, which log the tolerated exception with exc_info.
    Unconfigured, structlog formats those with rich, whose box-drawing characters raise
    UnicodeEncodeError on the cp1252 console of the Windows test hosts.
    """
    structlog.configure(
        processors=[
            structlog.dev.ConsoleRenderer(
                colors=False, exception_formatter=structlog.dev.plain_traceback
            )
        ]
    )


def tearDownModule():
    structlog.reset_defaults()


def test_result_line(label, shard=0):
    return json.dumps(
        {
            "id": {"testResult": {"label": label, "run": 1, "shard": shard, "attempt": 1}},
            "testResult": {"status": "PASSED", "testActionOutput": []},
        }
    )


def test_summary_line(label):
    return json.dumps(
        {
            "id": {"testSummary": {"label": label}},
            "testSummary": {"overallStatus": "PASSED"},
        }
    )


def started_line(uuid="10929faf-4aed-48ba-83bd-58d136dcf79c"):
    return json.dumps({"id": {"started": {}}, "started": {"uuid": uuid}})


def progress_line():
    return json.dumps({"id": {"progress": {"opaqueCount": 1}}, "progress": {"stderr": "noise"}})


class TestBEPParsing(unittest.TestCase):
    def test_summary_label(self):
        self.assertEqual(
            parse_test_summary_label(json.loads(test_summary_line("//foo:bar"))), "//foo:bar"
        )
        self.assertIsNone(parse_test_summary_label(json.loads(test_result_line("//foo:bar"))))
        self.assertIsNone(parse_test_summary_label(json.loads(progress_line())))

    def test_is_filtered_event(self):
        self.assertTrue(is_filtered_event(json.loads(test_result_line("//a:b"))))
        self.assertTrue(is_filtered_event(json.loads(test_summary_line("//a:b"))))
        self.assertTrue(is_filtered_event(json.loads(started_line())))
        self.assertFalse(is_filtered_event(json.loads(progress_line())))


class TestBEPTail(unittest.TestCase):
    def setUp(self):
        fd, self.path = tempfile.mkstemp()
        os.close(fd)
        self.addCleanup(os.unlink, self.path)
        self.tail = BEPTail(self.path)

    def append(self, data):
        with open(self.path, "a") as f:
            f.write(data)

    def test_incremental_reads(self):
        self.append('{"a":1}\n{"b":2}\n')
        self.assertEqual(self.tail.read_new_lines(), ['{"a":1}', '{"b":2}'])
        self.assertEqual(self.tail.read_new_lines(), [])
        self.append('{"c":3}\n')
        self.assertEqual(self.tail.read_new_lines(), ['{"c":3}'])

    def test_partial_trailing_line_left_for_next_poll(self):
        self.append('{"a":1}\n{"partial"')
        self.assertEqual(self.tail.read_new_lines(), ['{"a":1}'])
        self.append(":2}\n")
        self.assertEqual(self.tail.read_new_lines(), ['{"partial":2}'])


class TestActivationRouter(unittest.TestCase):
    def make_router(self, known=("//a:a", "//b:b"), activate=None):
        self.uploads = []
        self.activations = []

        def upload(lines):
            self.uploads.append(list(lines))

        def default_activate(labels):
            self.activations.append(set(labels))
            return set(labels)

        return ActivationRouter(set(known), upload, activate or default_activate)

    def test_activates_completed_labels_once(self):
        router = self.make_router()
        router.ingest([test_result_line("//a:a"), test_summary_line("//a:a")])
        router.flush()
        self.assertEqual(self.activations, [{"//a:a"}])
        # A repeat flush with no new completions does nothing.
        router.flush()
        self.assertEqual(self.activations, [{"//a:a"}])
        self.assertEqual(router.activated, {"//a:a"})

    def test_snapshot_uploaded_before_activation_and_filtered(self):
        order = []
        router = ActivationRouter(
            {"//a:a"},
            upload_snapshot=lambda lines: order.append(("upload", list(lines))),
            activate=lambda labels: (order.append(("activate", set(labels))), labels)[1],
        )
        router.ingest(
            [
                started_line(),
                progress_line(),
                test_result_line("//a:a"),
                test_summary_line("//a:a"),
            ]
        )
        router.flush()
        self.assertEqual(order[0][0], "upload")
        self.assertEqual(order[1], ("activate", {"//a:a"}))
        # Progress events are filtered out of the snapshot.
        uploaded = [json.loads(line) for line in order[0][1]]
        self.assertTrue(all(is_filtered_event(e) for e in uploaded))
        self.assertEqual(len(uploaded), 3)

    def test_upload_failure_blocks_activation_and_retries(self):
        upload_calls = []

        def upload(lines):
            upload_calls.append(1)
            if len(upload_calls) == 1:
                raise RuntimeError("s3 down")

        router = ActivationRouter({"//a:a"}, upload, lambda labels: set(labels))
        router.ingest([test_summary_line("//a:a")])
        router.flush()
        self.assertEqual(router.pending, {"//a:a"})  # Not activated on failed upload.
        router.flush()  # Retry succeeds.
        self.assertEqual(router.pending, set())
        self.assertEqual(router.activated, {"//a:a"})

    def test_activation_failure_retried_next_flush(self):
        calls = []

        def activate(labels):
            calls.append(set(labels))
            if len(calls) == 1:
                raise RuntimeError("evergreen 500")
            return set(labels)

        router = self.make_router(activate=activate)
        router.ingest([test_summary_line("//a:a")])
        router.flush()
        self.assertEqual(router.pending, {"//a:a"})
        router.ingest([test_summary_line("//b:b")])
        router.flush()
        self.assertEqual(calls[1], {"//a:a", "//b:b"})
        self.assertEqual(router.pending, set())

    def test_partial_activation_success(self):
        router = self.make_router(activate=lambda labels: {"//a:a"})
        router.ingest([test_summary_line("//a:a"), test_summary_line("//b:b")])
        router.flush()
        self.assertEqual(router.activated, {"//a:a"})
        self.assertEqual(router.pending, {"//b:b"})

    def test_unknown_label_not_activated(self):
        router = self.make_router(known=("//a:a",))
        router.ingest([test_summary_line("//stranger:task")])
        router.flush()
        self.assertEqual(self.activations, [])
        self.assertEqual(router.pending, set())

    def test_unparseable_line_skipped(self):
        router = self.make_router()
        router.ingest(["not json", test_summary_line("//a:a")])
        router.flush()
        self.assertEqual(self.activations, [{"//a:a"}])


class TestWatch(unittest.TestCase):
    def test_final_pass_after_bazel_exits(self):
        """
        Events appearing right before/at bazel exit are still processed.

        Instead of a real bazel process, the `alive = set([1])` is used to mock the bazel process exit.
        """
        fd, path = tempfile.mkstemp()
        os.close(fd)
        self.addCleanup(os.unlink, path)

        activations = []
        router = ActivationRouter(
            {"//a:a"},
            lambda lines: None,
            lambda labels: (activations.append(set(labels)), labels)[1],
        )

        def sleep(_seconds):
            # bazel "exits" and its last events land while the watcher sleeps.
            with open(path, "a") as f:
                f.write(test_summary_line("//a:a") + "\n")
            alive.clear()

        alive = set([1])
        with patch(
            "buildscripts.stream_result_task_activation._pid_alive",
            side_effect=lambda pid: bool(alive),
        ):
            watch(BEPTail(path), router, bazel_pid=999999, poll_seconds=0, sleep=sleep)
        self.assertEqual(activations, [{"//a:a"}])


class TestMakeActivator(unittest.TestCase):
    def _task(self, name, activated):
        task = MagicMock()
        task.display_name = name
        task.task_id = f"id-{name}"
        task.activated = activated
        return task

    def test_batch_activates_inactive_tasks(self):
        evg_api = MagicMock()
        tasks = {"//a:a": self._task("//a:a", False), "//b:b": self._task("//b:b", False)}
        activate = make_activator(evg_api, tasks, "version1", "variant1", execution=0)
        self.assertEqual(activate({"//a:a", "//b:b"}), {"//a:a", "//b:b"})
        evg_api.activate_version_tasks.assert_called_once_with(
            "version1", [{"name": "variant1", "tasks": ["//a:a", "//b:b"]}]
        )
        evg_api.restart_task.assert_not_called()

    def test_already_activated_skipped_on_first_execution(self):
        evg_api = MagicMock()
        tasks = {"//a:a": self._task("//a:a", True)}
        activate = make_activator(evg_api, tasks, "v", "bv", execution=0)
        self.assertEqual(activate({"//a:a"}), {"//a:a"})
        evg_api.restart_task.assert_not_called()
        evg_api.activate_version_tasks.assert_not_called()

    def test_already_activated_restarted_on_runner_restart(self):
        evg_api = MagicMock()
        tasks = {"//a:a": self._task("//a:a", True)}
        activate = make_activator(evg_api, tasks, "v", "bv", execution=1)
        self.assertEqual(activate({"//a:a"}), {"//a:a"})
        evg_api.restart_task.assert_called_once_with("id-//a:a")

    def test_handled_labels_are_recorded_for_the_end_of_run_pass(self):
        """The record is what stops the end-of-run pass restarting these a second time."""
        record = os.path.join(tempfile.mkdtemp(), "streamed.txt")
        evg_api = MagicMock()
        tasks = {
            "//a:a": self._task("//a:a", False),
            "//b:b": self._task("//b:b", True),
        }
        activate = make_activator(evg_api, tasks, "v", "bv", execution=1, record_path=record)
        activate({"//a:a"})
        activate({"//b:b"})
        self.assertEqual(get_streamed_labels(record), {"//a:a", "//b:b"})

    def test_nothing_recorded_without_a_record_path(self):
        evg_api = MagicMock()
        tasks = {"//a:a": self._task("//a:a", False)}
        activate = make_activator(evg_api, tasks, "v", "bv", execution=0)
        self.assertEqual(activate({"//a:a"}), {"//a:a"})


class TestEndOfRunSkipsStreamedTasks(unittest.TestCase):
    """The runner's end-of-run pass must not re-handle what the watcher already did.

    On a runner restart both paths restart already-activated tasks, so without the skip the
    end-of-run pass would restart every streamed task a second time and discard the run the
    watcher started — leaving the streaming with no effect at all.
    """

    def _run(self, streamed, execution):
        tasks = []
        for name in ("//a:a", "//b:b"):
            task = MagicMock()
            task.display_name = name
            task.task_id = f"id-{name}"
            task.activated = True
            task.tags = []
            tasks.append(task)

        evg_api = MagicMock()
        evg_api.version_by_id.return_value.build_variants_map = {"bv": "build1"}
        evg_api.tasks_by_build.return_value = tasks

        with patch(
            "buildscripts.evergreen_activate_result_tasks.get_streamed_labels",
            return_value=streamed,
        ):
            activate_result_task_group(
                "bv", "resmoke_tests", "v", evg_api, allow_restart=execution > 0
            )
        return evg_api

    def test_streamed_tasks_not_restarted_again(self):
        evg_api = self._run({"//a:a"}, execution=1)
        evg_api.restart_task.assert_called_once_with("id-//b:b")

    def test_all_restarted_when_nothing_was_streamed(self):
        evg_api = self._run(set(), execution=1)
        self.assertEqual(
            sorted(c.args[0] for c in evg_api.restart_task.call_args_list),
            ["id-//a:a", "id-//b:b"],
        )


class TestGetStreamedLabels(unittest.TestCase):
    def test_missing_file_means_nothing_was_streamed(self):
        self.assertEqual(get_streamed_labels("/nonexistent/streamed.txt"), set())

    def test_blank_lines_ignored(self):
        fd, path = tempfile.mkstemp()
        self.addCleanup(os.unlink, path)
        with os.fdopen(fd, "w") as f:
            f.write("//a:a\n\n//b:b\n")
        self.assertEqual(get_streamed_labels(path), {"//a:a", "//b:b"})


if __name__ == "__main__":
    unittest.main()
