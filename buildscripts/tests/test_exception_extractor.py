import unittest

import buildscripts.resmokelib.logging.handlers as under_test


class TestExceptionExtractor(unittest.TestCase):
    def get_exception_extractor(self, truncate=under_test.Truncate.FIRST):
        return under_test.ExceptionExtractor("START", "END", truncate)

    def get_js_exception_extractor(self):
        return under_test.ExceptionExtractor(
            r"^uncaught exception:",
            r"^exiting with code|^Failure detected from Mocha",
            under_test.Truncate.LAST,
        )

    def get_mocha_exception_extractor(self):
        return under_test.ExceptionExtractor(
            r"Test Report Summary:",
            r"failing tests detected",
            under_test.Truncate.LAST,
        )

    def test_successful_extraction(self):
        logs = [
            "not captured",
            "START",
            "captured",
            "END",
            "not captured",
        ]
        expected_exception = [
            "START",
            "captured",
            "END",
        ]
        exception_extractor = self.get_exception_extractor()
        for log in logs:
            exception_extractor.process_log_line(log)

        assert exception_extractor.exception_detected is True
        assert exception_extractor.get_exception() == expected_exception

    def test_partial_extraction(self):
        logs = [
            "not captured",
            "START",
            "captured",
        ]
        expected_current_exception = [
            "START",
            "captured",
        ]
        exception_extractor = self.get_exception_extractor()
        for log in logs:
            exception_extractor.process_log_line(log)

        assert exception_extractor.active is True
        assert exception_extractor.exception_detected is False
        assert list(exception_extractor.current_exception) == expected_current_exception
        assert not exception_extractor.get_exception()

    def test_no_extraction(self):
        logs = [
            "not captured",
            "not captured",
        ]
        expected_current_exception = []
        exception_extractor = self.get_exception_extractor()
        for log in logs:
            exception_extractor.process_log_line(log)

        assert exception_extractor.active is False
        assert exception_extractor.exception_detected is False
        assert list(exception_extractor.current_exception) == expected_current_exception
        assert not exception_extractor.get_exception()

    def test_successful_extraction_truncate_first(self):
        logs = (
            ["START"]
            + ["not captured"]
            + ["captured"] * (under_test.DEFAULT_MAX_EXCEPTION_LENGTH - 1)
            + ["END"]
        )
        expected_exception = (
            ["[LAST Part of Exception]"]
            + ["captured"] * (under_test.DEFAULT_MAX_EXCEPTION_LENGTH - 1)
            + ["END"]
        )
        exception_extractor = self.get_exception_extractor()
        for log in logs:
            exception_extractor.process_log_line(log)

        assert exception_extractor.exception_detected is True
        assert exception_extractor.get_exception() == expected_exception

    def test_successful_extraction_truncate_last(self):
        logs = (
            ["START"]
            + ["captured"] * (under_test.DEFAULT_MAX_EXCEPTION_LENGTH - 1)
            + ["not captured"]
            + ["END"]
        )
        expected_exception = (
            ["[FIRST Part of Exception]"]
            + ["START"]
            + ["captured"] * (under_test.DEFAULT_MAX_EXCEPTION_LENGTH - 1)
        )
        exception_extractor = self.get_exception_extractor(under_test.Truncate.LAST)
        for log in logs:
            exception_extractor.process_log_line(log)

        assert exception_extractor.exception_detected is True
        assert exception_extractor.get_exception() == expected_exception

    def test_js_extractor_ends_on_exiting_with_code(self):
        """The JS extractor still works for the traditional non-mocha path."""
        logs = [
            "some other output",
            "uncaught exception: Error: assert.eq failed",
            "doassert@src/mongo/shell/assert.js:47:14",
            "exiting with code 253",
            "not captured",
        ]
        expected = [
            "uncaught exception: Error: assert.eq failed",
            "doassert@src/mongo/shell/assert.js:47:14",
            "exiting with code 253",
        ]
        extractor = self.get_js_exception_extractor()
        for log in logs:
            extractor.process_log_line(log)

        assert extractor.exception_detected is True
        assert extractor.get_exception() == expected

    def test_js_extractor_ends_on_mocha_failure(self):
        """The JS extractor terminates on 'Failure detected from Mocha' for mocha tests."""
        logs = [
            "[jsTest] some passing test output",
            "uncaught exception: Error: 1 failing tests detected :",
            "report@jstests/libs/mochalite.js:126:19",
            "runTests@jstests/libs/mochalite.js:554:18",
            "Error: 1 failing tests detected",
            "Failure detected from Mocha test runner",
            "not captured",
        ]
        expected = [
            "uncaught exception: Error: 1 failing tests detected :",
            "report@jstests/libs/mochalite.js:126:19",
            "runTests@jstests/libs/mochalite.js:554:18",
            "Error: 1 failing tests detected",
            "Failure detected from Mocha test runner",
        ]
        extractor = self.get_js_exception_extractor()
        for log in logs:
            extractor.process_log_line(log)

        assert extractor.exception_detected is True
        assert extractor.get_exception() == expected

    def test_mocha_extractor_captures_summary(self):
        """The mocha extractor captures from 'Test Report Summary:' to 'failing tests detected'."""
        logs = [
            "[jsTest] some passing test output",
            "[jsTest] Test Report Summary:",
            "[jsTest]   3 passing",
            "[jsTest]   1 failing",
            "[jsTest] Failures and stacks are reprinted below.",
            "[jsTest] ----",
            "[jsTest] ✘ my test > should work",
            "[jsTest] Error: assert failed",
            "[jsTest] stack@line:1",
            "uncaught exception: Error: 1 failing tests detected :",
            "not captured",
        ]
        expected = [
            "[jsTest] Test Report Summary:",
            "[jsTest]   3 passing",
            "[jsTest]   1 failing",
            "[jsTest] Failures and stacks are reprinted below.",
            "[jsTest] ----",
            "[jsTest] ✘ my test > should work",
            "[jsTest] Error: assert failed",
            "[jsTest] stack@line:1",
            "uncaught exception: Error: 1 failing tests detected :",
        ]
        extractor = self.get_mocha_exception_extractor()
        for log in logs:
            extractor.process_log_line(log)

        assert extractor.exception_detected is True
        assert extractor.get_exception() == expected

    def test_mocha_extractor_truncates_long_failure(self):
        """When mocha failure output is long, the summary header (first lines) is preserved."""
        # Generate enough lines to exceed DEFAULT_MAX_EXCEPTION_LENGTH (150).
        num_filler = under_test.DEFAULT_MAX_EXCEPTION_LENGTH + 10
        logs = (
            ["[jsTest] Test Report Summary:"]
            + ["[jsTest]   36 passing"]
            + ["[jsTest]   1 failing"]
            + ["[jsTest] Failures and stacks are reprinted below."]
            + ["[jsTest] line %d" % i for i in range(num_filler)]
            + ["uncaught exception: Error: 1 failing tests detected :"]
        )
        extractor = self.get_mocha_exception_extractor()
        for log in logs:
            extractor.process_log_line(log)

        assert extractor.exception_detected is True
        result = extractor.get_exception()
        # Truncated to DEFAULT_MAX_EXCEPTION_LENGTH, keeping first lines (Truncate.LAST),
        # plus the "[FIRST Part of Exception]" label prepended.
        assert len(result) == under_test.DEFAULT_MAX_EXCEPTION_LENGTH + 1  # +1 for truncation label
        assert result[0] == "[FIRST Part of Exception]"
        assert result[1] == "[jsTest] Test Report Summary:"
        assert result[2] == "[jsTest]   36 passing"
        assert result[3] == "[jsTest]   1 failing"

    def test_custom_max_length(self):
        """ExceptionExtractor respects a custom max_length parameter."""
        custom_max = 5
        extractor = under_test.ExceptionExtractor(
            "START", "END", under_test.Truncate.LAST, max_length=custom_max
        )
        logs = ["START"] + ["line %d" % i for i in range(10)] + ["END"]
        for log in logs:
            extractor.process_log_line(log)

        assert extractor.exception_detected is True
        result = extractor.get_exception()
        # Truncated to custom_max, plus the truncation label
        assert len(result) == custom_max + 1
        assert result[0] == "[FIRST Part of Exception]"
        assert result[1] == "START"

    def test_mocha_extractor_no_match_on_passing_test(self):
        """The mocha extractor does not fire when all tests pass (no 'failing tests detected')."""
        logs = [
            "[jsTest] Test Report Summary:",
            "[jsTest]   3 passing",
            "[jsTest] ----",
            "some other output",
        ]
        extractor = self.get_mocha_exception_extractor()
        for log in logs:
            extractor.process_log_line(log)

        # start_regex matched so the extractor is active, but end_regex never fired
        assert extractor.active is True
        assert extractor.exception_detected is False
        assert extractor.get_exception() == []
