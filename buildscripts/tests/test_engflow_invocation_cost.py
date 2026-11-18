"""Unit tests for buildscripts/engflow_invocation_cost.py."""

import json
import os
import tempfile
import unittest
from unittest.mock import MagicMock

from buildscripts import engflow_invocation_cost as under_test


def worker_time(pool: str, seconds: float) -> dict:
    """Build the part of an EngFlow profile worker_time event that the estimate reads."""
    return {
        "ph": "X",
        "cat": "worker_time",
        "name": "//some:target TestRunner abc",
        "dur": int(seconds * 1_000_000),
        "args": {"action_pool": pool},
    }


class TestBlendedRate(unittest.TestCase):
    def test_default_rate_blends_spot_and_on_demand(self):
        self.assertAlmostEqual(0.157, under_test.blended_usd_per_vcpu_hour())

    def test_rate_follows_the_module_variables(self):
        self.assertAlmostEqual(
            0.6,
            under_test.blended_usd_per_vcpu_hour(
                spot_usd_per_vcpu_hour=0.2, on_demand_usd_per_vcpu_hour=1.0, spot_fraction=0.5
            ),
        )


class TestSummarizeWorkerTime(unittest.TestCase):
    def test_sums_worker_time_per_pool(self):
        events = [
            worker_time("test_runner_arm64_2core", 1800),
            worker_time("test_runner_arm64_2core", 1800),
            worker_time("default", 3600),
        ]

        usage = under_test.summarize_worker_time(events)

        self.assertEqual(2, usage["test_runner_arm64_2core"].actions)
        self.assertAlmostEqual(3600, usage["test_runner_arm64_2core"].worker_seconds)
        self.assertEqual(1, usage["default"].actions)
        self.assertAlmostEqual(3600, usage["default"].worker_seconds)

    def test_ignores_events_that_are_not_worker_time(self):
        events = [
            {"ph": "X", "cat": "none", "name": "downloadBlob", "dur": 5_000_000, "args": {}},
            {"ph": "M", "name": "process_name", "args": {"name": "worker"}},
            {"ph": "X", "name": "execution_time", "dur": 5_000_000},
        ]

        self.assertEqual({}, under_test.summarize_worker_time(events))


class TestPoolCost(unittest.TestCase):
    def test_cost_scales_with_the_vcpus_of_the_pool(self):
        one_hour = under_test.PoolUsage(actions=1, worker_seconds=3600)

        self.assertAlmostEqual(0.157, under_test.pool_cost_usd("test_runner_arm64_1core", one_hour))
        self.assertAlmostEqual(0.314, under_test.pool_cost_usd("test_runner_arm64_2core", one_hour))
        self.assertAlmostEqual(0.628, under_test.pool_cost_usd("test_runner_arm64_4core", one_hour))

    def test_unknown_pool_is_priced_with_the_fallback_vcpus(self):
        one_hour = under_test.PoolUsage(actions=1, worker_seconds=3600)

        self.assertEqual(under_test.UNKNOWN_POOL_VCPUS, under_test.pool_vcpus("brand_new_pool"))
        self.assertAlmostEqual(
            0.157 * under_test.UNKNOWN_POOL_VCPUS,
            under_test.pool_cost_usd("brand_new_pool", one_hour),
        )


def build_metrics(remote: int) -> dict:
    """Build the BEP event that reports how many actions each runner executed."""
    return {
        "id": {"buildMetrics": {}},
        "buildMetrics": {
            "actionSummary": {
                "runnerCount": [
                    {"name": "total", "count": remote + 7},
                    {"name": "remote cache hit", "count": 5, "execKind": "Remote"},
                    {"name": "internal", "count": 2, "execKind": "Local"},
                    {"name": "remote", "count": remote, "execKind": "Remote"},
                ]
            }
        },
    }


STARTED = {"id": {"started": {}}, "started": {"uuid": "1111-2222"}}


class TestReadBep(unittest.TestCase):
    def _write_bep(self, events: list[dict]) -> str:
        handle = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False)
        self.addCleanup(os.unlink, handle.name)
        with handle:
            for event in events:
                handle.write(json.dumps(event) + "\n")
        return handle.name

    def test_reads_the_invocation_id_and_the_remote_action_count(self):
        path = self._write_bep(
            [STARTED, {"id": {"progress": {}}, "progress": {}}, build_metrics(18)]
        )

        bep = under_test.read_bep(path)

        self.assertEqual("1111-2222", bep.invocation_id)
        self.assertEqual(18, bep.remote_actions)

    def test_invocation_without_remote_actions_expects_none(self):
        metrics = build_metrics(0)
        metrics["buildMetrics"]["actionSummary"]["runnerCount"].pop()
        path = self._write_bep([STARTED, metrics])

        self.assertEqual(0, under_test.read_bep(path).remote_actions)

    def test_remote_action_count_is_unknown_without_build_metrics(self):
        # A bazel that was killed never writes its build metrics.
        path = self._write_bep([STARTED])

        bep = under_test.read_bep(path)

        self.assertEqual("1111-2222", bep.invocation_id)
        self.assertIsNone(bep.remote_actions)

    def test_missing_file_has_no_invocation(self):
        self.assertIsNone(under_test.read_bep("/nonexistent/build_events.json"))

    def test_bep_without_a_started_event_has_no_invocation(self):
        path = self._write_bep([{"id": {"progress": {}}, "progress": {}}])

        self.assertIsNone(under_test.read_bep(path))

    def test_truncated_line_is_skipped(self):
        path = self._write_bep([STARTED])
        with open(path, "a") as bep:
            bep.write('{"id": {"buildMetrics": {}}, "buildMetr')

        self.assertEqual("1111-2222", under_test.read_bep(path).invocation_id)


class TestFetchTraceEvents(unittest.TestCase):
    def _response(self, status_code: int, body: dict | None = None) -> MagicMock:
        response = MagicMock()
        response.status_code = status_code
        response.json.return_value = body
        return response

    def test_requests_the_profile_of_the_invocation(self):
        session = MagicMock()
        session.get.return_value = self._response(200, {"traceEvents": [{"ph": "M"}]})

        events = under_test.fetch_trace_events(session, "1111-2222", sleep=lambda _: None)

        self.assertEqual([{"ph": "M"}], events)
        self.assertEqual(
            "https://sodalite.cluster.engflow.com/api/profiling/v1/instances/default/invocations/1111-2222",
            session.get.call_args.args[0],
        )

    def test_retries_until_the_profile_is_served(self):
        session = MagicMock()
        session.get.side_effect = [
            self._response(404),
            self._response(200, {"traceEvents": []}),
        ]

        self.assertEqual(
            [], under_test.fetch_trace_events(session, "1111-2222", sleep=lambda _: None)
        )
        self.assertEqual(2, session.get.call_count)

    def test_gives_up_after_the_last_attempt(self):
        session = MagicMock()
        session.get.return_value = self._response(403)

        with self.assertRaises(under_test.ProfileUnavailable):
            under_test.fetch_trace_events(session, "1111-2222", sleep=lambda _: None)
        self.assertEqual(under_test.FETCH_ATTEMPTS, session.get.call_count)


class TestFetchInvocationUsage(unittest.TestCase):
    def _fetch(self, profiles: list[list[dict]], remote_actions: int | None):
        """Fetch the usage of an invocation whose profile grows through `profiles`."""
        remaining = list(profiles)
        sleeps: list[float] = []
        usage = under_test.fetch_invocation_usage(
            MagicMock(),
            under_test.BepInfo("1111-2222", remote_actions),
            fetch=lambda session, invocation_id: remaining.pop(0),
            sleep=sleeps.append,
        )
        return usage, sleeps

    def test_complete_profile_is_fetched_once(self):
        usage, sleeps = self._fetch([[worker_time("default", 60), worker_time("default", 60)]], 2)

        self.assertEqual(2, usage.pools["default"].actions)
        self.assertEqual(2, usage.expected_actions)
        self.assertEqual([], sleeps)

    def test_waits_for_engflow_to_record_the_last_actions(self):
        # EngFlow adds the actions that finished last to the profile after the invocation ends.
        early = [worker_time("default", 60)]
        settled = early + [worker_time("default", 600)]

        usage, sleeps = self._fetch([early, early, settled], 2)

        self.assertEqual(2, usage.pools["default"].actions)
        self.assertAlmostEqual(660, usage.pools["default"].worker_seconds)
        self.assertEqual([under_test.PROFILE_POLL_SECONDS] * 2, sleeps)

    def test_more_recorded_actions_than_bazel_counted_is_complete(self):
        # Retries and the remote half of dynamically scheduled actions add worker time.
        usage, sleeps = self._fetch([[worker_time("default", 60)] * 3], 2)

        self.assertEqual(3, usage.pools["default"].actions)
        self.assertEqual([], sleeps)

    def test_gives_up_waiting_and_keeps_the_partial_profile(self):
        early = [worker_time("default", 60)]

        usage, sleeps = self._fetch([early] * under_test.PROFILE_FETCHES, 2)

        self.assertEqual(1, usage.pools["default"].actions)
        self.assertEqual(2, usage.expected_actions)
        self.assertEqual(under_test.PROFILE_FETCHES - 1, len(sleeps))

    def test_unknown_remote_action_count_does_not_wait(self):
        usage, sleeps = self._fetch([[worker_time("default", 60)]], None)

        self.assertEqual(1, usage.pools["default"].actions)
        self.assertIsNone(usage.expected_actions)
        self.assertEqual([], sleeps)


class TestFormatReport(unittest.TestCase):
    def test_reports_the_total_and_each_pool(self):
        invocations = [
            under_test.InvocationUsage(
                "build-uuid", {"default": under_test.PoolUsage(actions=10, worker_seconds=3600)}
            ),
            under_test.InvocationUsage(
                "test-uuid",
                {
                    "test_runner_arm64_2core": under_test.PoolUsage(
                        actions=4, worker_seconds=2 * 3600
                    ),
                    "test_runner_arm64_1core": under_test.PoolUsage(actions=2, worker_seconds=3600),
                },
            ),
        ]

        report = under_test.format_report(invocations)
        lines = report.splitlines()

        # 1 + 4 + 1 = 6 vCPU-hours at $0.157.
        self.assertIn("$0.94", lines[0])
        self.assertIn("6.0 vCPU-hours", lines[0])
        self.assertIn("$0.157", lines[0])
        self.assertIn("70% spot at $0.13", lines[0])
        self.assertIn("30% on demand at $0.22", lines[0])
        self.assertIn("build-uuid", report)
        self.assertIn("test-uuid", report)
        # Pools are listed most expensive first within an invocation.
        self.assertLess(
            report.index("test_runner_arm64_2core"), report.index("test_runner_arm64_1core")
        )

    def test_reports_pools_priced_with_the_fallback(self):
        invocations = [
            under_test.InvocationUsage(
                "uuid", {"brand_new_pool": under_test.PoolUsage(actions=1, worker_seconds=3600)}
            )
        ]

        self.assertIn("brand_new_pool", under_test.format_report(invocations).splitlines()[-1])

    def test_reports_an_invocation_engflow_has_not_finished_recording(self):
        invocations = [
            under_test.InvocationUsage(
                "uuid",
                {"default": under_test.PoolUsage(actions=15, worker_seconds=3600)},
                expected_actions=18,
            )
        ]

        report = under_test.format_report(invocations)

        self.assertIn("15 of 18 remote actions", report)
        self.assertIn("at least $0.16", report.splitlines()[0])

    def test_complete_invocation_is_not_reported_as_a_lower_bound(self):
        invocations = [
            under_test.InvocationUsage(
                "uuid",
                {"default": under_test.PoolUsage(actions=18, worker_seconds=3600)},
                expected_actions=18,
            )
        ]

        report = under_test.format_report(invocations)

        self.assertNotIn("at least", report)
        self.assertNotIn("remote actions", report)

    def test_invocation_without_remote_actions_costs_nothing(self):
        report = under_test.format_report([under_test.InvocationUsage("uuid", {})])

        self.assertIn("$0.00", report.splitlines()[0])


if __name__ == "__main__":
    unittest.main()
