"""End-to-end tests for buildscripts/bazel_burn_in.py."""

import json
import os
import platform
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import call, patch

MAX_BAZEL_BUILD_RETRIES = 3
INITIAL_BAZEL_BUILD_RETRY_DELAY_SECONDS = 1


def _run_bazel_build_with_backoff() -> subprocess.CompletedProcess[str]:
    build_command = [
        "bazel",
        "build",
        "//...",
        "--build_tag_filters=resmoke_config",
        "--config=local",
    ]

    for attempt in range(MAX_BAZEL_BUILD_RETRIES + 1):
        build_result = subprocess.run(build_command, capture_output=True, text=True)
        if build_result.returncode == 0:
            return build_result

        if attempt == MAX_BAZEL_BUILD_RETRIES:
            return build_result

        backoff_seconds = INITIAL_BAZEL_BUILD_RETRY_DELAY_SECONDS * (2**attempt)
        print(
            "Bazel build failed with exit code "
            f"{build_result.returncode} (attempt {attempt + 1}/{MAX_BAZEL_BUILD_RETRIES + 1}); "
            f"retrying in {backoff_seconds}s..."
        )
        time.sleep(backoff_seconds)

    raise AssertionError("unreachable")


@unittest.skipUnless(
    platform.system() == "Linux" and platform.machine().lower() not in {"ppc64le", "s390x"},
    "Burn-in task generation only runs on x86/arm Linux",
)
class TestBazelBurnInEnd2End(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        print("\nBuilding resmoke configs with bazel...")
        build_result = _run_bazel_build_with_backoff()
        if build_result.returncode != 0:
            raise RuntimeError(
                "Failed to build resmoke configs with bazel after "
                f"{MAX_BAZEL_BUILD_RETRIES + 1} attempts:\n"
                f"stdout: {build_result.stdout}\n"
                f"stderr: {build_result.stderr}"
            )

        print("Generating resmoke_suite_configs.yml...")
        cquery_result = subprocess.run(
            [
                "bazel",
                "cquery",
                "kind(resmoke_config, //jstests/suites/...)",  # A subset of reality (//...), to speed up this test's runtime.
                "--output=starlark",
                "--starlark:expr",
                "': '.join([str(target.label).replace('@@','')] + [f.path for f in target.files.to_list()])",
            ],
            capture_output=True,
            text=True,
        )

        if cquery_result.returncode != 0:
            raise RuntimeError(
                f"Failed to query resmoke configs with bazel:\n"
                f"stdout: {cquery_result.stdout}\n"
                f"stderr: {cquery_result.stderr}"
            )

        with open("resmoke_suite_configs.yml", "w") as f:
            f.write(cquery_result.stdout)

    @classmethod
    def tearDownClass(cls):
        """Clean up resmoke_suite_configs.yml file after tests complete."""
        if os.path.exists("resmoke_suite_configs.yml"):
            os.remove("resmoke_suite_configs.yml")

    def test_generate_tasks(self):
        mock_changed_files = "jstests/core/js/jssymbol.js"

        with tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False) as f:
            outfile = f.name

        try:
            print("Generating tasks...")
            process = subprocess.run(
                [
                    sys.executable,
                    "buildscripts/bazel_burn_in.py",
                    "generate-tasks",
                    "abc",  # Is effectively ignored since --test-changed-files is used
                    "--outfile",
                    outfile,
                    "--test-changed-files",
                    mock_changed_files,
                ],
                text=True,
                capture_output=True,
            )

            self.assertEqual(
                0,
                process.returncode,
                f"bazel_burn_in.py generate-tasks failed with stderr: {process.stderr}",
            )

            self.assertTrue(os.path.exists(outfile), "Output file should be created")

            with open(outfile, "r") as f:
                try:
                    output_json = json.load(f)
                    self.assertIsNotNone(output_json, "Output should be valid JSON")

                    self.assertIn(
                        "buildvariants", output_json, "JSON should contain 'buildvariants'"
                    )
                    self.assertIn("tasks", output_json, "JSON should contain 'tasks'")

                    buildvariants = output_json["buildvariants"]
                    tasks = output_json["tasks"]

                    self.assertIsInstance(buildvariants, list, "buildvariants should be a list")
                    self.assertIsInstance(tasks, list, "tasks should be a list")

                    # Property 1: tasks exist for the changed test (jssymbol.js)
                    found_burn_in_for_test = False
                    for task in tasks:
                        if "burn_in" in task["name"]:
                            commands = task.get("commands", [])
                            for cmd in commands:
                                if cmd.get("func") == "execute resmoke tests via bazel":
                                    targets = cmd.get("vars", {}).get("targets", "")
                                    if "jssymbol.js" in targets and "burn_in" in targets:
                                        found_burn_in_for_test = True
                                        break
                        if found_burn_in_for_test:
                            break

                    self.assertTrue(
                        found_burn_in_for_test,
                        "Should have burn-in tasks for the changed test file (jssymbol.js)",
                    )

                    # Property 2: no duplicate tasks
                    task_names = [task["name"] for task in tasks]
                    unique_task_names = set(task_names)
                    self.assertEqual(
                        len(task_names),
                        len(unique_task_names),
                        f"Found duplicate task names: {[name for name in task_names if task_names.count(name) > 1]}",
                    )

                    # Property 3: no duplicate build variants
                    variant_names = [variant["name"] for variant in buildvariants]
                    unique_variant_names = set(variant_names)
                    self.assertEqual(
                        len(variant_names),
                        len(unique_variant_names),
                        f"Found duplicate variant names: {[name for name in variant_names if variant_names.count(name) > 1]}",
                    )

                except json.JSONDecodeError as e:
                    self.fail(f"Output file does not contain valid JSON: {e}")

        finally:
            if os.path.exists(outfile):
                os.remove(outfile)

    def test_generate_targets(self):
        import buildscripts.bazel_burn_in as under_test

        mock_changed_files = "jstests/core/js/jssymbol.js"

        # Snapshot BUILD.bazel files that generate-targets will modify so we
        # can restore them afterward without clobbering unrelated user changes.
        targets = under_test.query_targets_to_burn_in("abc", mock_changed_files)
        affected_files = set()
        for target in targets:
            build_file, _ = under_test.parse_bazel_target(target.original_target)
            affected_files.add(build_file)

        originals = {}
        for path in affected_files:
            if os.path.exists(path):
                with open(path, "r") as f:
                    originals[path] = f.read()

        try:
            print("Running generate-targets...")
            result = subprocess.run(
                [
                    sys.executable,
                    "buildscripts/bazel_burn_in.py",
                    "generate-targets",
                    "abc",
                    "--test-changed-files",
                    mock_changed_files,
                ],
                text=True,
                capture_output=True,
            )

            self.assertEqual(
                0,
                result.returncode,
                f"generate-targets failed with stderr: {result.stderr}",
            )
        finally:
            # Restore only the files we touched.
            for path, content in originals.items():
                with open(path, "w") as f:
                    f.write(content)


class TestRunBazelBuildWithBackoff(unittest.TestCase):
    @patch("buildscripts.tests.burn_in.test_bazel_burn_in_end2end.time.sleep")
    @patch("buildscripts.tests.burn_in.test_bazel_burn_in_end2end.subprocess.run")
    def test_retries_until_success(self, run_mock, sleep_mock):
        run_mock.side_effect = [
            subprocess.CompletedProcess(
                args=["bazel"], returncode=1, stdout="", stderr="failed once"
            ),
            subprocess.CompletedProcess(
                args=["bazel"], returncode=1, stdout="", stderr="failed twice"
            ),
            subprocess.CompletedProcess(args=["bazel"], returncode=0, stdout="success", stderr=""),
        ]

        result = _run_bazel_build_with_backoff()
        expected_backoffs = [
            call(INITIAL_BAZEL_BUILD_RETRY_DELAY_SECONDS * (2**attempt)) for attempt in range(2)
        ]

        self.assertEqual(0, result.returncode)
        self.assertEqual(3, run_mock.call_count)
        self.assertEqual(2, sleep_mock.call_count)
        sleep_mock.assert_has_calls(expected_backoffs)

    @patch("buildscripts.tests.burn_in.test_bazel_burn_in_end2end.time.sleep")
    @patch("buildscripts.tests.burn_in.test_bazel_burn_in_end2end.subprocess.run")
    def test_returns_last_failure_after_max_retries(self, run_mock, sleep_mock):
        failures = [
            subprocess.CompletedProcess(
                args=["bazel"], returncode=1, stdout="", stderr=f"failed {idx}"
            )
            for idx in range(MAX_BAZEL_BUILD_RETRIES + 1)
        ]
        run_mock.side_effect = failures

        result = _run_bazel_build_with_backoff()
        expected_backoffs = [
            call(INITIAL_BAZEL_BUILD_RETRY_DELAY_SECONDS * (2**attempt))
            for attempt in range(MAX_BAZEL_BUILD_RETRIES)
        ]

        self.assertEqual(1, result.returncode)
        self.assertEqual(MAX_BAZEL_BUILD_RETRIES + 1, run_mock.call_count)
        self.assertEqual(MAX_BAZEL_BUILD_RETRIES, sleep_mock.call_count)
        sleep_mock.assert_has_calls(expected_backoffs)


if __name__ == "__main__":
    unittest.main()
