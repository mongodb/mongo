import subprocess
import unittest
from unittest import mock

from buildscripts.streams_test_timeout_linter import (
    changed_line_numbers,
    find_violations_in_text,
    has_timeout_signal,
    is_suppressed,
)


class TestSuppression(unittest.TestCase):
    def test_nolint_suppresses(self):
        self.assertTrue(is_suppressed("sleepFor(Milliseconds(100));  // NOLINT"))
        self.assertTrue(is_suppressed("sleepFor(Milliseconds(100));  // NO LINT"))

    def test_no_suppression(self):
        self.assertFalse(is_suppressed("sleepFor(Milliseconds(100));"))


class TestTimeoutSignal(unittest.TestCase):
    def test_keywords(self):
        for line in (
            "sleepFor(Milliseconds(1));",
            "waitForCondition(..., Seconds(1));",
            ".connectTimeout = Seconds(5),",
            ".baseWaitMs = Milliseconds(100),",
            ".cacheEntryTTL = Milliseconds(60000),",
            "auto deadline = Date_t::now() + Seconds(5);",
            "ASSERT_EVENTUALLY(cond);",
            "ASSERT_WITHIN(Seconds(5), cond);",
        ):
            self.assertTrue(has_timeout_signal(line), msg=line)

    def test_poll_loop(self):
        self.assertTrue(has_timeout_signal("while (timer.elapsed() < Minutes(1)) {"))
        self.assertTrue(
            has_timeout_signal(
                "    while (getNumUnpublishedFiles() != 0 && timer.elapsed() < Milliseconds(4200)) {"
            )
        )

    def test_non_timeout_lines(self):
        for line in (
            "tickSource.advance(Seconds(1));",
            "ASSERT_EQ(timer.elapsed() - now, Seconds(0));",
            "const auto minTestDuration = Seconds(4);",
            "return kBuildOverhead * ::mongo::Microseconds(n);",
        ):
            self.assertFalse(has_timeout_signal(line), msg=line)


class TestFindViolations(unittest.TestCase):
    def test_flags_bare_timeout_durations(self):
        text = "\n".join(
            [
                "sleepFor(Milliseconds(100));",
                ".connectTimeout = Seconds(5),",
                "while (timer.elapsed() < Minutes(1)) {",
                "auto deadline = Date_t::now() + Seconds(5);",
                "auto deadline = Date_t::now() + Seconds{5};",
            ]
        )
        self.assertEqual(len(find_violations_in_text(text)), 5)

    def test_accepts_timeout_qualified(self):
        text = "\n".join(
            [
                "sleepFor(test::duration::Seconds(1) + test::duration::Milliseconds(100));",
                ".connectTimeout = ::streams::test::duration::Seconds(5),",
                "ASSERT_WITHIN(test::duration::Seconds(5), [&] { return ready(); });",
            ]
        )
        self.assertEqual(find_violations_in_text(text), [])

    def test_leaves_non_timeout_durations(self):
        text = "\n".join(
            [
                "tickSource.advance(Seconds(1));",
                "ASSERT_EQ(timer.elapsed(), Seconds(1));",
            ]
        )
        self.assertEqual(find_violations_in_text(text), [])

    def test_assertion_comparison_not_flagged(self):
        text = "\n".join(
            [
                "ASSERT_EQ(mongo::Milliseconds(2000), sourceConfig.clientConfig.retryConfig.baseWaitMs);",
                "ASSERT_EQ(mongo::Seconds(30), sourceConfig.clientConfig.connectTimeout);",
            ]
        )
        self.assertEqual(find_violations_in_text(text), [])

    def test_nolint_suppresses_violation(self):
        text = "sleepFor(Milliseconds(100));  // NOLINT\n"
        self.assertEqual(find_violations_in_text(text), [])

    def test_mongo_qualified_is_bare(self):
        text = ".connectTimeout = mongo::Seconds(5),\n"
        self.assertEqual(len(find_violations_in_text(text)), 1)

    def test_qualified_and_bare_on_same_line_flags_bare(self):
        # The qualified call is fine; the bare constructor on the same line is still flagged.
        text = "sleepFor(test::duration::Seconds(1) + Milliseconds(100));\n"
        self.assertEqual(len(find_violations_in_text(text)), 1)

    def test_one_violation_per_line(self):
        # Multiple bare constructors on one line produce a single violation.
        text = (
            ".maxRetries = 0, .baseWaitMs = Milliseconds(100), .maxWaitMs = Milliseconds(1000)};\n"
        )
        self.assertEqual(len(find_violations_in_text(text)), 1)


class TestChangedLineNumbers(unittest.TestCase):
    _DIFF = (
        "@@ -10,3 +10,4 @@\n"
        " context1\n"
        " context2\n"
        "+added line\n"
        "@@ -20,2 +21,2 @@\n"
        "-removed\n"
        "+replaced\n"
        " context3\n"
    )

    def test_parses_added_and_modified_lines(self):
        fake = subprocess.CompletedProcess(args=[], returncode=0, stdout=self._DIFF, stderr="")
        with mock.patch(
            "buildscripts.streams_test_timeout_linter.subprocess.run", return_value=fake
        ):
            changed = changed_line_numbers("origin/master", "some/file.cpp")
        # "added line" is new-file line 12; "replaced" is new-file line 21.
        self.assertEqual(changed, {12, 21})

    def test_empty_diff(self):
        fake = subprocess.CompletedProcess(args=[], returncode=0, stdout="", stderr="")
        with mock.patch(
            "buildscripts.streams_test_timeout_linter.subprocess.run", return_value=fake
        ):
            self.assertEqual(changed_line_numbers("origin/master", "some/file.cpp"), set())


if __name__ == "__main__":
    unittest.main()
