"""Unit tests for validate_evg_project_config.py"""

import unittest

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


# 'foo' optional comma + whitespace
