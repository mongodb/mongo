import unittest

import buildscripts.resmokelib.logging.handlers as under_test


class TestExceptionExtractor(unittest.TestCase):
    def get_exception_extractor(self, truncate=under_test.Truncate.FIRST):
        return under_test.ExceptionExtractor("START", "END", truncate)

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
            + ["captured"] * (under_test.MAX_EXCEPTION_LENGTH - 1)
            + ["END"]
        )
        expected_exception = (
            ["[LAST Part of Exception]"]
            + ["captured"] * (under_test.MAX_EXCEPTION_LENGTH - 1)
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
            + ["captured"] * (under_test.MAX_EXCEPTION_LENGTH - 1)
            + ["not captured"]
            + ["END"]
        )
        expected_exception = (
            ["[FIRST Part of Exception]"]
            + ["START"]
            + ["captured"] * (under_test.MAX_EXCEPTION_LENGTH - 1)
        )
        exception_extractor = self.get_exception_extractor(under_test.Truncate.LAST)
        for log in logs:
            exception_extractor.process_log_line(log)

        assert exception_extractor.exception_detected is True
        assert exception_extractor.get_exception() == expected_exception
