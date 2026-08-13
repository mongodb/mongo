"""Unit tests for generate_tss_test_list."""

import contextlib
import io
import os
import pathlib
import sys
import tempfile
import unittest
from unittest.mock import patch

import requests
import yaml

sys.path.append(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from bazel.resmoke import generate_tss_test_list as under_test
from buildscripts.resmokelib import config as _config


class TestDiscoverTests(unittest.TestCase):
    def test_flattens_multiple_documents(self):
        # test-discovery emits a multi-document stream when given several suites.
        output = yaml.safe_dump_all(
            [
                {"suite_name": "suite_a", "tests": ["a.js", "b.js"]},
                {"suite_name": "suite_b", "tests": ["c.js"]},
            ]
        )
        with patch.object(under_test.cli, "main", side_effect=lambda argv: print(output)):
            tests = under_test.discover_tests("suite.yml", [], [])
        self.assertEqual(["a.js", "b.js", "c.js"], tests)

    def test_forwards_tag_arguments(self):
        seen = {}

        def fake_main(argv):
            seen["argv"] = argv
            print(yaml.safe_dump({"suite_name": "s", "tests": []}))

        with patch.object(under_test.cli, "main", side_effect=fake_main):
            under_test.discover_tests("suite.yml", ["tags.yml", "more.yml"], ["t1,t2"])

        self.assertEqual(
            [
                "resmoke.py",
                "test-discovery",
                "--suite",
                "suite.yml",
                "--tagFile",
                "tags.yml",
                "--tagFile",
                "more.yml",
                "--excludeWithAnyTags",
                "t1,t2",
            ],
            seen["argv"],
        )

    def test_does_not_enable_test_selection(self):
        # Asking TSS while building the list to ask TSS about would be circular.
        seen = {}

        def fake_main(argv):
            seen["argv"] = argv
            print(yaml.safe_dump({"suite_name": "s", "tests": []}))

        with patch.object(under_test.cli, "main", side_effect=fake_main):
            under_test.discover_tests("suite.yml", [], [])

        self.assertNotIn("--enableEvergreenApiTestSelection", seen["argv"])


class TestReport(unittest.TestCase):
    """The build log is the only place a developer sees whether this action worked."""

    def _report(self, tests, failed, discovered=None):
        stderr = io.StringIO()
        discovered = len(tests) if discovered is None else discovered
        with contextlib.redirect_stderr(stderr):
            under_test.report("//pkg:suite", "out.yml", tests, discovered, failed)
        return stderr.getvalue()

    def test_success_reports_the_count_and_the_path(self):
        output = self._report([f"t{i}.js" for i in range(25)], failed=False)
        self.assertIn("Discovered 25 tests for //pkg:suite", output)
        self.assertIn("out.yml", output)

    def test_test_names_are_not_echoed(self):
        # The file has the full list; the log only needs the counts and where to find them.
        output = self._report(["a.js", "b.js"], failed=False)
        self.assertNotIn("a.js", output)
        self.assertNotIn("b.js", output)
        self.assertEqual(1, len(output.strip().splitlines()))

    def test_failure_is_reported_as_failure(self):
        output = self._report([], failed=True)
        self.assertIn("FAILED", output)
        self.assertIn("//pkg:suite", output)

    def test_a_narrowed_list_reports_both_counts(self):
        # The file holds the selection, so the discovered count has to be stated separately or
        # the line contradicts the selection message that follows it.
        output = self._report(["a.js"], failed=False, discovered=1492)
        self.assertIn("Discovered 1492 tests", output)
        self.assertIn("wrote the selected 1", output)

    def test_empty_list_without_failure_warns(self):
        output = self._report([], failed=False)
        self.assertIn("WARNING", output)
        self.assertIn("0 tests", output)


MAINLINE = {
    "project": "mongodb-mongo-master",
    "build_variant": "amazon-linux2023-arm64-static-compile",
    "task_name": "resmoke_tests",
    "build_id": (
        "mongodb_mongo_master_amazon_linux2023_arm64_static_compile"
        "_aa312d94e50c9e429d157ab2ae9dcf40f62f1258_26_08_10_13_32_05"
    ),
}

PATCH = {
    "project": "mongodb-mongo-master",
    "build_variant": "promote-sys-perf-build",
    "task_name": "promote_sys_perf_build",
    "build_id": (
        "mongodb_mongo_master_promote_sys_perf_build_patch"
        "_238c5d20ff0586f2ec829e431de5808348d2c63d_6a79dddb29e7b30007ee6a44_26_08_10_14_19_25"
    ),
}


class TestConstructTaskId(unittest.TestCase):
    """Ids are checked against real Evergreen task ids; a wrong id silently misses the cache."""

    def test_mainline_label_keeps_colon_and_converts_slashes(self):
        task_id = under_test.construct_task_id(
            MAINLINE, "//buildscripts/smoke_tests/server_collection_write_path:concurrency"
        )
        self.assertEqual(
            "mongodb_mongo_master_amazon_linux2023_arm64_static_compile"
            "___buildscripts_smoke_tests_server_collection_write_path:concurrency"
            "_aa312d94e50c9e429d157ab2ae9dcf40f62f1258_26_08_10_13_32_05",
            task_id,
        )

    def test_patch_keeps_patch_marker_revision_and_version(self):
        # The task name is spliced in before "_patch_", not after it.
        task_id = under_test.construct_task_id(PATCH, "promote_sys_perf_build")
        self.assertEqual(
            "mongodb_mongo_master_promote_sys_perf_build_promote_sys_perf_build_patch"
            "_238c5d20ff0586f2ec829e431de5808348d2c63d_6a79dddb29e7b30007ee6a44_26_08_10_14_19_25",
            task_id,
        )

    def test_unexpected_build_id_yields_no_id(self):
        volatile = dict(MAINLINE, build_id="something_else_entirely")
        self.assertEqual("", under_test.construct_task_id(volatile, "//pkg:suite"))

    def test_missing_volatile_status_yields_no_id(self):
        self.assertEqual("", under_test.construct_task_id({}, "//pkg:suite"))


class TestTaskIdSelfCheck(unittest.TestCase):
    """The runner's own id is the only ground truth available at build time."""

    def test_recognizes_a_format_it_can_reproduce(self):
        volatile = dict(PATCH, task_id=under_test.construct_task_id(PATCH, PATCH["task_name"]))
        self.assertTrue(under_test.task_id_format_is_understood(volatile))

    def test_rejects_a_format_it_cannot_reproduce(self):
        volatile = dict(PATCH, task_id="mongodb_mongo_master_something_we_did_not_predict")
        self.assertFalse(under_test.task_id_format_is_understood(volatile))

    def test_rejects_missing_task_id(self):
        self.assertFalse(under_test.task_id_format_is_understood(PATCH))


class TestSelectTests(unittest.TestCase):
    """Selection talks to TSS over the mesh; every failure must raise so the caller fails open."""

    @contextlib.contextmanager
    def _tss(self, status_code=200, payload=None):
        response = unittest.mock.MagicMock()
        response.json.return_value = payload
        response.status_code = status_code
        if status_code >= 400:
            response.raise_for_status.side_effect = requests.HTTPError(f"{status_code} error")
        response.history = []
        response.is_redirect = False
        response.is_permanent_redirect = False
        session = unittest.mock.MagicMock()
        session.post.return_value = response
        with (
            patch.object(under_test, "tss_session", return_value=session),
            patch.object(under_test, "find_tss_client_secret", return_value="secret"),
        ):
            yield session

    def test_selection_can_only_narrow_never_add(self):
        volatile = dict(MAINLINE, task_id="x", requester="patch_request")
        with self._tss(payload=["a.js", "not_in_suite.js"]):
            tests = under_test.select_tests(
                ["a.js", "b.js"], "//pkg:suite", volatile, ["Optimistic"]
            )
        self.assertEqual(["a.js"], tests)

    def test_posts_to_the_environment_url(self):
        # Spelled out rather than built from the constants: the address and route are the
        # service's contract, so a change to either should have to be made here too.
        volatile = dict(MAINLINE, task_id="x", requester="patch_request")
        with self._tss(payload=[]) as session:
            under_test.select_tests(["a.js"], "//pkg:suite", volatile, ["Optimistic"])
        self.assertEqual(
            "https://test-selection-services.cloud-build.prod.corp.mongodb.com"
            "/api/test_selection/select_tests/",
            session.post.call_args[0][0],
        )

    def test_environment_defaults_to_production(self):
        with patch.object(under_test, "read_expansions", return_value={}):
            self.assertEqual("prod", under_test.tss_environment())

    def test_staging_is_opt_in_by_patch_parameter(self):
        for value in ("true", "True", "1", "yes", "on", " true "):
            with self.subTest(value=value):
                expansions = {under_test.TSS_STAGING_PARAM: value}
                with patch.object(under_test, "read_expansions", return_value=expansions):
                    self.assertEqual("staging", under_test.tss_environment())

    def test_falsey_or_absent_parameter_stays_on_production(self):
        for expansions in ({}, {"use_tss_staging": ""}, {"use_tss_staging": "false"}):
            with self.subTest(expansions=expansions):
                with patch.object(under_test, "read_expansions", return_value=expansions):
                    self.assertEqual("prod", under_test.tss_environment())

    def test_every_environment_has_a_url(self):
        self.assertEqual({"staging", "prod"}, set(under_test.TSS_BASE_URLS))

    def test_sends_the_constructed_task_id_not_the_runner_one(self):
        volatile = dict(MAINLINE, task_id="the_runner_task", requester="patch_request")
        with self._tss(payload=[]) as session:
            under_test.select_tests(["a.js"], "//pkg:suite", volatile, ["Optimistic"])
        sent = session.post.call_args[1]["json"]
        self.assertEqual(under_test.construct_task_id(volatile, "//pkg:suite"), sent["task_id"])
        self.assertEqual("//pkg:suite", sent["task_name"])
        self.assertEqual(["Optimistic"], sent["strategies"])

    def test_http_error_quotes_the_body_so_the_cause_is_visible(self):
        # A validation error names the fields it rejected; that body is the whole diagnosis.
        volatile = dict(MAINLINE, task_id="x")
        with self._tss(status_code=422) as session:
            session.post.return_value.text = '{"detail":[{"loc":["body","tests"]}]}'
            with self.assertRaises(RuntimeError) as caught:
                under_test.select_tests(["a.js"], "//pkg:suite", volatile, ["Optimistic"])
        self.assertIn("422", str(caught.exception))
        self.assertIn('"loc":["body","tests"]', str(caught.exception))

    def test_a_wrapped_list_is_rejected_rather_than_silently_dropping_tests(self):
        # The service returns a bare list. An object would make set() iterate its keys, which
        # would look like a successful selection of nothing rather than an error.
        volatile = dict(MAINLINE, task_id="x")
        with self._tss(payload={"tests": ["a.js"]}):
            with self.assertRaises(RuntimeError):
                under_test.select_tests(["a.js"], "//pkg:suite", volatile, ["Optimistic"])

    def test_non_list_response_raises_so_caller_fails_open(self):
        volatile = dict(MAINLINE, task_id="x")
        with self._tss(payload="nonsense"):
            with self.assertRaises(RuntimeError):
                under_test.select_tests(["a.js"], "//pkg:suite", volatile, ["Optimistic"])

    def test_redirect_is_reported_instead_of_being_followed(self):
        # Following the service's slash redirect lands on http:// port 80 and times out, so the
        # redirect itself has to be the error.
        volatile = dict(MAINLINE, task_id="x")
        with self._tss(payload=[]) as session:
            session.post.return_value.is_redirect = True
            session.post.return_value.status_code = 301
            session.post.return_value.headers = {
                "Location": "http://test-selection-services/api/test_selection/select_tests/"
            }
            with self.assertRaises(RuntimeError) as caught:
                under_test.select_tests(["a.js"], "//pkg:suite", volatile, ["Optimistic"])
        self.assertIn("301", str(caught.exception))
        self.assertIn("http://test-selection-services", str(caught.exception))

    def test_redirects_are_not_followed(self):
        volatile = dict(MAINLINE, task_id="x")
        with self._tss(payload=[]) as session:
            under_test.select_tests(["a.js"], "//pkg:suite", volatile, ["Optimistic"])
        self.assertIs(False, session.post.call_args[1]["allow_redirects"])

    def test_non_json_body_is_quoted_in_the_error(self):
        # A 2xx whose body is not JSON means we reached something other than the endpoint, and
        # the body is the only clue as to what.
        volatile = dict(MAINLINE, task_id="x")
        with self._tss(payload=None) as session:
            session.post.return_value.json.side_effect = ValueError("no json")
            session.post.return_value.text = "<html>login</html>"
            session.post.return_value.headers = {"Content-Type": "text/html"}
            with self.assertRaises(RuntimeError) as caught:
                under_test.select_tests(["a.js"], "//pkg:suite", volatile, ["Optimistic"])
        self.assertIn("<html>login</html>", str(caught.exception))
        self.assertIn("text/html", str(caught.exception))

    def test_missing_client_secret_raises_so_caller_fails_open(self):
        volatile = dict(MAINLINE, task_id="x")
        with patch.object(under_test, "find_tss_client_secret", return_value=""):
            with self.assertRaises(RuntimeError):
                under_test.select_tests(["a.js"], "//pkg:suite", volatile, ["Optimistic"])


class TestTssSession(unittest.TestCase):
    """Only the network call is mocked, so the real credentials model is exercised here."""

    @contextlib.contextmanager
    def _okta(self, token):
        oauth_session = unittest.mock.MagicMock()
        oauth_session.fetch_token.return_value = token
        with patch.object(under_test, "OAuth2Session", return_value=oauth_session) as constructor:
            yield oauth_session, constructor

    def test_builds_a_session_with_a_bearer_header(self):
        token = {"access_token": "the-token", "expires_in": 3600}
        with self._okta(token):
            session = under_test.tss_session("secret")
        self.assertEqual("Bearer the-token", session.headers["Authorization"])

    def test_requests_the_expected_client_and_scope(self):
        token = {"access_token": "the-token", "expires_in": 3600}
        with self._okta(token) as (oauth_session, _):
            under_test.tss_session("secret")
        kwargs = oauth_session.fetch_token.call_args[1]
        self.assertEqual(under_test.TSS_OAUTH_CLIENT_ID, kwargs["client_id"])
        self.assertEqual("secret", kwargs["client_secret"])
        self.assertEqual(under_test.TSS_OAUTH_SCOPE, kwargs["scope"])
        self.assertIn("aus4k4jv00hWjNnps297", kwargs["token_url"])


class TestIsSelectable(unittest.TestCase):
    """Some suites have no test files for selection to reason about."""

    def _write(self, contents):
        with tempfile.NamedTemporaryFile("w", suffix=".yml", delete=False) as suite_file:
            suite_file.write(contents)
        self.addCleanup(os.unlink, suite_file.name)
        return suite_file.name

    def test_a_suite_of_files_is_selectable(self):
        path = self._write(yaml.safe_dump({"test_kind": "js_test", "selector": {"roots": []}}))
        self.assertTrue(under_test.is_selectable(path))

    def test_a_mongos_test_suite_is_not(self):
        # resmoke hands back the selector config as the single test case, not a list of files.
        path = self._write(yaml.safe_dump({"test_kind": "mongos_test", "selector": {"test": ""}}))
        self.assertFalse(under_test.is_selectable(path))

    def test_an_unreadable_config_is_left_to_discovery_to_report(self):
        self.assertTrue(under_test.is_selectable("/nonexistent/suite.yml"))

    def test_valid_yaml_that_is_not_a_mapping_is_left_to_discovery_to_report(self):
        # A crash here would escape main()'s fail-open handling and leave the genrule with no
        # output file at all, which is worse than letting discovery report the bad suite.
        for contents in ("[]\n", "- one\n- two\n", "just a scalar\n", "\n"):
            with self.subTest(contents=contents):
                self.assertTrue(under_test.is_selectable(self._write(contents)))


class TestMongosTestSuiteIsSkipped(unittest.TestCase):
    def test_writes_a_disabled_file_without_running_discovery(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            output = os.path.join(tmpdir, "tests.yml")
            suite = os.path.join(tmpdir, "mongos_test.yml")
            pathlib.Path(suite).write_text(yaml.safe_dump({"test_kind": "mongos_test"}))
            argv = [
                "generate_tss_test_list.py",
                "--suite",
                suite,
                "--label",
                "//src/mongo/s:mongos_test",
                "--output",
                output,
            ]
            with (
                patch.object(sys, "argv", argv),
                patch.object(under_test, "discover_tests") as discover,
                contextlib.redirect_stderr(io.StringIO()),
            ):
                under_test.main()
            discover.assert_not_called()
            with open(output) as written:
                self.assertEqual(
                    {
                        "suite": "//src/mongo/s:mongos_test",
                        "status": "disabled",
                        "tests": [],
                    },
                    yaml.safe_load(written),
                )


class TestReadSelectionSettings(unittest.TestCase):
    """Enable and strategies come from the build settings file written by _tss_settings_file."""

    def _read(self, contents):
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as settings:
            settings.write(contents)
        try:
            return under_test.read_selection_settings(settings.name)
        finally:
            os.unlink(settings.name)

    def test_enabled_with_explicit_strategies(self):
        self.assertEqual(
            (True, ["NotPassing", "Optimistic"]), self._read("true\nNotPassing,Optimistic\n")
        )

    def test_enabled_with_no_strategies_uses_resmokes_default(self):
        # The whole point: the two flows agree on the default without repeating it.
        enabled, strategies = self._read("true\n\n")
        self.assertTrue(enabled)
        self.assertEqual(_config.DEFAULT_EVERGREEN_TEST_SELECTION_STRATEGY, strategies)

    def test_disabled(self):
        self.assertEqual((False, ["ExcludeManuallyQuarantined"]), self._read("false\n\n"))

    def test_missing_file_is_disabled(self):
        self.assertEqual((False, []), under_test.read_selection_settings("/nonexistent.txt"))

    def test_no_path_is_disabled(self):
        self.assertEqual((False, []), under_test.read_selection_settings(""))

    def test_whitespace_is_tolerated(self):
        self.assertEqual((True, ["Optimistic"]), self._read(" TRUE \n  Optimistic , \n"))


class TestFindTssClientSecret(unittest.TestCase):
    def test_environment_is_preferred(self):
        with patch.dict(os.environ, {under_test.TSS_CLIENT_SECRET_KEY: "from-env"}):
            self.assertEqual("from-env", under_test.find_tss_client_secret())

    def test_falls_back_to_expansions(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            workspace = pathlib.Path(tmpdir) / "src"
            workspace.mkdir()
            (pathlib.Path(tmpdir) / "expansions.yml").write_text(
                yaml.safe_dump({under_test.TSS_CLIENT_SECRET_KEY: "from-expansions"})
            )
            with (
                patch.dict(os.environ, {}, clear=True),
                patch.object(under_test, "find_workspace_root", return_value=workspace),
            ):
                self.assertEqual("from-expansions", under_test.find_tss_client_secret())

    def test_absent_everywhere_returns_empty(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            workspace = pathlib.Path(tmpdir) / "src"
            workspace.mkdir()
            with (
                patch.dict(os.environ, {}, clear=True),
                patch.object(under_test, "find_workspace_root", return_value=workspace),
            ):
                self.assertEqual("", under_test.find_tss_client_secret())


class TestSelectionSummaryLine(unittest.TestCase):
    """The summary must not claim narrowing when the selection kept everything."""

    def _run(self, discovered_tests, selected_tests):
        with tempfile.TemporaryDirectory() as tmpdir:
            output = os.path.join(tmpdir, "tests.yml")
            settings = os.path.join(tmpdir, "settings.txt")
            pathlib.Path(settings).write_text("true\nOptimistic\n")
            argv = [
                "generate_tss_test_list.py",
                "--suite",
                "suite.yml",
                "--label",
                "//pkg:suite",
                "--output",
                output,
                "--selection-settings",
                settings,
                "--volatile-status",
                "/nonexistent",
            ]
            stderr = io.StringIO()
            with (
                patch.object(sys, "argv", argv),
                patch.object(under_test, "discover_tests", return_value=discovered_tests),
                patch.object(under_test, "parse_volatile_status", return_value={"task_id": "t"}),
                patch.object(under_test, "task_id_format_is_understood", return_value=True),
                patch.object(under_test, "select_tests", return_value=selected_tests),
                patch.object(under_test, "tss_environment", return_value="staging"),
                contextlib.redirect_stderr(stderr),
            ):
                under_test.main()
            return stderr.getvalue()

    def test_says_it_did_not_narrow_when_nothing_was_removed(self):
        output = self._run(["a.js", "b.js"], ["a.js", "b.js"])
        self.assertIn("did not narrow down any of the 2 tests", output)
        self.assertNotIn("narrowed //pkg:suite", output)

    def test_reports_the_reduction_when_tests_were_removed(self):
        output = self._run(["a.js", "b.js"], ["a.js"])
        self.assertIn("narrowed //pkg:suite from 2 to 1 tests", output)


class TestMainFailsOpen(unittest.TestCase):
    """A broken discovery step must never be able to cause tests to be skipped."""

    def _run_main(self, side_effect):
        with tempfile.TemporaryDirectory() as tmpdir:
            output = os.path.join(tmpdir, "tests.yml")
            argv = [
                "generate_tss_test_list.py",
                "--suite",
                "suite.yml",
                "--label",
                "//pkg:suite",
                "--output",
                output,
            ]
            with (
                patch.object(sys, "argv", argv),
                patch.object(under_test.cli, "main", side_effect=side_effect),
            ):
                under_test.main()
            with open(output) as written:
                return yaml.safe_load(written)

    def test_exception_yields_empty_list(self):
        result = self._run_main(RuntimeError("discovery blew up"))
        self.assertEqual({"suite": "//pkg:suite", "status": "failed", "tests": []}, result)

    def test_system_exit_yields_empty_list(self):
        # resmoke's argument parsing exits rather than raising.
        result = self._run_main(SystemExit(2))
        self.assertEqual({"suite": "//pkg:suite", "status": "failed", "tests": []}, result)

    def test_success_writes_discovered_tests(self):
        result = self._run_main(
            lambda argv: print(yaml.safe_dump({"suite_name": "s", "tests": ["a.js"]}))
        )
        self.assertEqual({"suite": "//pkg:suite", "status": "disabled", "tests": ["a.js"]}, result)


if __name__ == "__main__":
    unittest.main()
