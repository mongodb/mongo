"""Unit tests for buildscripts/generate_result_tasks.py query construction."""

import json
import unittest
from unittest import mock

from buildscripts import generate_result_tasks as g

INCOMPATIBLE = "incompatible_with_bazel_remote_test"


class TestBuildTagQuery(unittest.TestCase):
    def test_rbe_excludes_incompatible(self):
        q = g._build_tag_query(["ci-default"], "//...", local_exec=False)
        # RBE subtracts the incompatible-tagged set and never intersects with it.
        self.assertIn(f"- (attr(tags, '\\b{INCOMPATIBLE}", q)
        self.assertNotIn("^", q)

    def test_local_exec_intersects_incompatible(self):
        q = g._build_tag_query(["ci-default"], "//...", local_exec=True)
        # Local exec keeps ONLY the incompatible-tagged set (intersection) and does not subtract it.
        self.assertIn("^", q)
        self.assertIn(INCOMPATIBLE, q)
        self.assertNotIn(f"- (attr(tags, '\\b{INCOMPATIBLE}", q)

    def test_local_exec_without_negative_tags_has_no_trailing_subtraction(self):
        q = g._build_tag_query(["ci-default"], "//...", local_exec=True)
        self.assertNotIn(" - (", q)  # no negatives -> no subtraction clause

    def test_local_exec_keeps_other_negative_tags(self):
        q = g._build_tag_query(["ci-default", "-flaky"], "//...", local_exec=True)
        self.assertIn("^", q)
        self.assertIn("- (attr(tags, '\\bflaky", q)
        # The incompatible tag is required, not subtracted.
        self.assertNotIn(f"- (attr(tags, '\\b{INCOMPATIBLE}", q)

    def test_multiple_positive_tags_are_unioned(self):
        q = g._build_tag_query(["ci-default", "ci-release-critical"], "//...", local_exec=False)
        self.assertIn("ci-default", q)
        self.assertIn("ci-release-critical", q)
        self.assertIn(" + ", q)


class TestExpandEvergreenVariables(unittest.TestCase):
    def test_plain_variable(self):
        self.assertEqual(g.expand_evergreen_variables("${a}", {"a": "x"}), "x")

    def test_missing_variable_without_default_is_empty(self):
        self.assertEqual(g.expand_evergreen_variables("${a}", {}), "")

    def test_default_used_when_unset(self):
        self.assertEqual(g.expand_evergreen_variables("${a|d}", {}), "d")

    def test_default_used_when_empty(self):
        self.assertEqual(g.expand_evergreen_variables("${a|d}", {"a": ""}), "d")

    def test_value_overrides_default(self):
        self.assertEqual(g.expand_evergreen_variables("${a|d}", {"a": "x"}), "x")

    def test_tag_filter_suffix_default_vs_override(self):
        text = "--test_tag_filters=${resmoke_tests_tag_filter}${resmoke_bazel_test_incompatible_filter|,-incompatible_with_bazel_remote_test}"
        rbe = g.expand_evergreen_variables(text, {"resmoke_tests_tag_filter": "ci-default"})
        self.assertEqual(rbe, "--test_tag_filters=ci-default,-incompatible_with_bazel_remote_test")
        local = g.expand_evergreen_variables(
            text,
            {
                "resmoke_tests_tag_filter": "ci-default",
                "resmoke_bazel_test_incompatible_filter": ",incompatible_with_bazel_remote_test",
            },
        )
        self.assertEqual(local, "--test_tag_filters=ci-default,incompatible_with_bazel_remote_test")


class TestMakeResultsTask(unittest.TestCase):
    @staticmethod
    def _exec_vars(task):
        for cmd in task["commands"]:
            if cmd.get("func") == "execute resmoke tests via bazel":
                return cmd.get("vars", {})
        return {}

    @staticmethod
    def _funcs(task):
        return [c.get("func") for c in task["commands"]]

    def test_rbe_result_task_fetches_remote_and_injects_nothing(self):
        task = g.make_results_task("//x:y", resmoke_disable_rbe=False)
        self.assertIn("fetch remote test results", self._funcs(task))
        self.assertNotIn("resmoke_disable_rbe", self._exec_vars(task))

    def test_local_result_task_gathers_local_and_self_marks(self):
        task = g.make_results_task("//x:y", resmoke_disable_rbe=True)
        self.assertIn("gather local test results", self._funcs(task))
        ev = self._exec_vars(task)
        self.assertEqual(ev["resmoke_disable_rbe"], "true")
        self.assertEqual(ev["resmoke_bazel_test_incompatible_filter"], g.LOCAL_INCOMPATIBLE_FILTER)
        self.assertEqual(g.LOCAL_INCOMPATIBLE_FILTER, ",incompatible_with_bazel_remote_test")


class TestMakeLocalResultsTask(unittest.TestCase):
    @staticmethod
    def _funcs(task):
        return [c.get("func") for c in task["commands"]]

    @staticmethod
    def _commands(task):
        return [c.get("func") or c.get("command") for c in task["commands"]]

    def setUp(self):
        self.task = g.make_local_results_task("//jstests/suites/security:auth")

    def test_named_after_target_and_tagged_local(self):
        self.assertEqual(self.task["name"], "//jstests/suites/security:auth")
        self.assertIn(g.LOCAL_TASK_TAG, self.task["tags"])
        self.assertEqual(g.LOCAL_TASK_TAG, "resmoke_local_test")

    def test_is_self_contained_setup_execute_gather(self):
        cmds = self._commands(self.task)
        # Sets up the workspace itself (not relying on a task group's setup_group).
        self.assertIn("git get project and add git tag", cmds)
        self.assertIn("set up venv", cmds)
        self.assertIn("s3.get", cmds)  # downloads the dist-test binaries
        # Then runs locally and gathers local results.
        self.assertIn("execute resmoke tests via bazel", cmds)
        self.assertIn("gather local test results", cmds)

    def test_execute_runs_locally_with_incompatible_filter(self):
        ev = next(
            c["vars"]
            for c in self.task["commands"]
            if c.get("func") == "execute resmoke tests via bazel"
        )
        self.assertEqual(ev["resmoke_disable_rbe"], "true")
        self.assertEqual(ev["result_task"], True)
        self.assertEqual(ev["resmoke_bazel_test_incompatible_filter"], g.LOCAL_INCOMPATIBLE_FILTER)
        self.assertEqual(ev["targets"], "//jstests/suites/security:auth")

    def test_no_remote_fetch(self):
        self.assertNotIn("fetch remote test results", self._funcs(self.task))

    def test_task_definition_carries_no_run_on(self):
        # run_on lives on the per-variant task ref, never on the shared standalone task definition.
        self.assertNotIn("run_on", self.task)


class _FakeVariant:
    def __init__(self, name, large_distro_name=None):
        self.name = name
        self._large = large_distro_name

    def expansion(self, key):
        return self._large if key == "large_distro_name" else None


class TestResolveLargeHostDistro(unittest.TestCase):
    def test_returns_none_when_not_tagged(self):
        variant = _FakeVariant("v", large_distro_name="big")
        tags = {"//local:small": ["other"]}
        self.assertIsNone(g.resolve_large_host_distro(variant, "//local:small", tags))

    def test_returns_variant_large_distro_when_tagged(self):
        variant = _FakeVariant("v", large_distro_name="big")
        tags = {"//local:big": [g.REQUIRES_LARGE_HOST_TAG]}
        self.assertEqual(g.resolve_large_host_distro(variant, "//local:big", tags), "big")

    def test_same_target_resolves_per_variant(self):
        # The same requires_large_host suite lands on each variant's own large distro.
        tags = {"//local:big": [g.REQUIRES_LARGE_HOST_TAG]}
        a = _FakeVariant("a", large_distro_name="distro-a")
        b = _FakeVariant("b", large_distro_name="distro-b")
        self.assertEqual(g.resolve_large_host_distro(a, "//local:big", tags), "distro-a")
        self.assertEqual(g.resolve_large_host_distro(b, "//local:big", tags), "distro-b")

    def test_raises_when_tagged_but_variant_lacks_large_distro(self):
        variant = _FakeVariant("v", large_distro_name=None)
        tags = {"//local:big": [g.REQUIRES_LARGE_HOST_TAG]}
        with self.assertRaisesRegex(RuntimeError, "large_distro_name"):
            g.resolve_large_host_distro(variant, "//local:big", tags)


class TestQueryTargetTags(unittest.TestCase):
    def test_parses_streamed_jsonproto_tags(self):
        lines = [
            json.dumps(
                {
                    "rule": {
                        "name": "//jstests/suites/replication:replica_sets",
                        "attribute": [
                            {"name": "shard_count", "intValue": 40},
                            {
                                "name": "tags",
                                "stringListValue": [
                                    "requires_large_host",
                                    "resources:memory:14336",
                                ],
                            },
                        ],
                    }
                }
            ),
            json.dumps({"rule": {"name": "@@//x:y", "attribute": []}}),
        ]
        completed = mock.Mock(stdout="\n".join(lines) + "\n")
        with mock.patch.object(g.subprocess, "run", return_value=completed):
            tags = g.query_target_tags(["//jstests/suites/replication:replica_sets", "//x:y"])
        self.assertEqual(
            tags["//jstests/suites/replication:replica_sets"],
            ["requires_large_host", "resources:memory:14336"],
        )
        # @@ / @ repo prefixes are stripped so keys match the query target names.
        self.assertEqual(tags["//x:y"], [])

    def test_empty_targets_skips_bazel(self):
        with mock.patch.object(g.subprocess, "run") as run:
            self.assertEqual(g.query_target_tags([]), {})
        run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
