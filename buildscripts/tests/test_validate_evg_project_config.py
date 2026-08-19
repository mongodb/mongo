"""Unit tests for validate_evg_project_config.py"""

import tempfile
import unittest
from pathlib import Path
from unittest import mock

from buildscripts import validate_evg_project_config
from buildscripts.validate_evg_project_config import messages_to_report


class TestMessagesToReport(unittest.TestCase):
    def test_allowed_if_not_in_all_projects(self):
        messages = [
            "NOTICE: task 'my_task' defined but not used by any variants; consider using or disabling"
        ]
        (_, shared_evg_validate_messages) = messages_to_report(messages, num_of_projects=2)
        self.assertEqual(
            shared_evg_validate_messages,
            messages,
            "The message should have been allowed in shared.",
        )

    def test_in_all_projects(self):
        messages = [
            "NOTICE: task 'my_task' defined but not used by any variants; consider using or disabling"
        ]
        (error_on_evg_validate_messages, _) = messages_to_report(messages, num_of_projects=1)
        self.assertEqual(
            error_on_evg_validate_messages,
            messages,
            "The message should have been reported as an error.",
        )

    def test_unmatched_tag_selector(self):
        messages = [
            "buildvariant 'foo' has unmatched criteria: '!.bar'",
            "buildvariant 'foo' has unmatched selector: '.bar .baz'",
        ]
        (error_on_evg_validate_messages, _) = messages_to_report(messages, num_of_projects=1)
        self.assertEqual(
            error_on_evg_validate_messages,
            [],
            "The messages should have been allowed.",
        )

    def test_unmatched_selector(self):
        messages = [
            "buildvariant 'foo' has unmatched criteria: 'bar', 'baz'",
            # "buildvariant 'foo' has unmatched selector: 'bar'",
        ]
        (error_on_evg_validate_messages, _) = messages_to_report(messages, num_of_projects=1)
        self.assertEqual(
            error_on_evg_validate_messages,
            messages,
            "The messages should have been reported as errors.",
        )

    def test_unmatched_selector_in_combo_message(self):
        messages = [
            "buildvariant 'foo' has unmatched criteria: '.bar .baz', 'bam'",
            "buildvariant 'foo' has unmatched selector: 'bar', '!.baz'",
        ]
        (error_on_evg_validate_messages, _) = messages_to_report(messages, num_of_projects=1)
        self.assertEqual(
            error_on_evg_validate_messages,
            messages,
            "The messages should have been reported as errors.",
        )


class TestAuthentication(unittest.TestCase):
    @mock.patch.object(
        validate_evg_project_config.subprocess,
        "run",
        return_value=validate_evg_project_config.subprocess.CompletedProcess([], 1),
    )
    def test_authentication_failure_has_distinct_exit_code(self, _run):
        with tempfile.TemporaryDirectory() as temp:
            auth_config = Path(temp) / "evergreen.yml"
            auth_config.write_text("oauth:\n  client_id: test\n", encoding="utf-8")
            with self.assertRaises(SystemExit) as raised:
                validate_evg_project_config.ensure_authenticated("evergreen", str(auth_config))

        self.assertEqual(
            raised.exception.code,
            validate_evg_project_config.AUTHENTICATION_FAILURE_EXIT_CODE,
        )


# 'foo' optional comma + whitespace

if __name__ == "__main__":
    unittest.main()
