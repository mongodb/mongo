import os
import platform
import subprocess
import sys
import unittest

import yaml

import buildscripts.burn_in_tests as under_test


@unittest.skipUnless(
    platform.system() == "Linux" and platform.machine().lower() not in {"ppc64le", "s390x"},
    "Burn-in task generation only runs on x86/arm Linux",
)
class TestBurnInTestsEnd2End(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(
            [
                sys.executable,
                "buildscripts/burn_in_tests.py",
                "generate-test-membership-map-file-for-ci",
            ],
            check=True,
        )

    @classmethod
    def tearDownClass(cls):
        if os.path.exists(under_test.BURN_IN_TEST_MEMBERSHIP_FILE):
            os.remove(under_test.BURN_IN_TEST_MEMBERSHIP_FILE)

    def test_changed_files(self):
        mock_changed_files = (
            "jstests/noPassthrough/shell/js/array.js,jstests/noPassthrough/shell/js/date.js"
        )
        process = subprocess.run(
            [
                sys.executable,
                "buildscripts/burn_in_tests.py",
                "run",
                "--yaml",
                "--test-changed-files",
                mock_changed_files,
            ],
            text=True,
            capture_output=True,
        )

        self.assertEqual(
            0,
            process.returncode,
            f"burn_in_tests.py failed with stderr: {process.stderr}",
        )

        output = process.stdout
        try:
            parsed_yaml = yaml.safe_load(output)
            self.assertIsNotNone(parsed_yaml, "Output should be valid YAML")

            # Verify the structure of the YAML output
            self.assertIn("discovered_tasks", parsed_yaml, "YAML should contain 'discovered_tasks'")
            discovered_tasks = parsed_yaml["discovered_tasks"]
            self.assertIsInstance(discovered_tasks, list, "discovered_tasks should be a list")

            # Verify we have at least one task discovered
            self.assertGreater(len(discovered_tasks), 0, "Should discover at least one task")

            # Verify each task has the expected structure
            for task in discovered_tasks:
                self.assertIn("task_name", task, "Each task should have 'task_name'")
                self.assertIn("suites", task, "Each task should have 'suites'")
                self.assertIsInstance(task["suites"], list, "Task suites should be a list")

                # Verify each suite has the expected structure
                for suite in task["suites"]:
                    self.assertIn("suite_name", suite, "Each suite should have 'suite_name'")
                    self.assertIn("test_list", suite, "Each suite should have 'test_list'")
                    self.assertIsInstance(
                        suite["test_list"], list, "Suite test_list should be a list"
                    )

            # Verify changed test files appear in the results
            all_tests = []
            for task in discovered_tasks:
                for suite in task["suites"]:
                    all_tests.extend(suite["test_list"])
            expected_tests = [
                "jstests/noPassthrough/shell/js/array.js",
                "jstests/noPassthrough/shell/js/date.js",
            ]
            for expected_test in expected_tests:
                self.assertIn(
                    expected_test,
                    all_tests,
                    f"Expected test {expected_test} should be in discovered tests",
                )

        except Exception as e:
            self.fail(f"burn_in_tests.py does not output valid yaml: {e}\nOutput: {output}")

    def test_non_test_files(self):
        mock_changed_files = "src/mongo/shell/mongo_main.cpp"

        process = subprocess.run(
            [
                sys.executable,
                "buildscripts/burn_in_tests.py",
                "run",
                "--yaml",
                "--test-changed-files",
                mock_changed_files,
            ],
            text=True,
            capture_output=True,
        )

        self.assertEqual(
            0,
            process.returncode,
            f"burn_in_tests.py failed with stderr: {process.stderr}",
        )

        output = process.stdout
        try:
            parsed_yaml = yaml.safe_load(output)
            self.assertIsNotNone(parsed_yaml, "Output should be valid YAML")

            self.assertIn("discovered_tasks", parsed_yaml, "YAML should contain 'discovered_tasks'")
            discovered_tasks = parsed_yaml["discovered_tasks"]
            self.assertIsInstance(discovered_tasks, list, "discovered_tasks should be a list")

            self.assertEqual(
                len(discovered_tasks),
                0,
                "Should have no discovered tasks for non-test files",
            )

        except Exception as e:
            self.fail(f"burn_in_tests.py does not output valid yaml: {e}\nOutput: {output}")


if __name__ == "__main__":
    unittest.main()
