"""Unit tests for legacy_commmands_check.py"""

import textwrap
import unittest
from typing import Iterable

from buildscripts.legacy_commands_check import check_file_for_legacy_type


def create_file_iterator(file_contents: str) -> Iterable[str]:
    return textwrap.dedent(file_contents.strip()).splitlines()


class TestCheckFileForLegacyType(unittest.TestCase):
    def test_typed_command(self):
        modified_lines = [(0, ""), (1, "class AddShardCmd : public TypedCommand")]
        self.assertEqual(check_file_for_legacy_type(modified_lines), False)

    def test_command(self):
        modified_lines = [(0, ""), (1, "class AddShardCmd : public Command {")]
        self.assertEqual(check_file_for_legacy_type(modified_lines), True)

    def test_basic_command(self):
        modified_lines = [(0, ""), (1, "class AddShardCmd : public BasicCommand")]
        self.assertEqual(check_file_for_legacy_type(modified_lines), True)

    def test_basic_command_with_reply_builder_interface(self):
        modified_lines = [
            (0, ""),
            (1, "class AddShardCmd : public BasicCommandWithReplyBuilderInterface"),
        ]
        self.assertEqual(check_file_for_legacy_type(modified_lines), True)

    def test_basic_command_with_request_parser(self):
        modified_lines = [(0, ""), (1, "class AddShardCmd : public BasicCommandWithRequestParser")]
        self.assertEqual(check_file_for_legacy_type(modified_lines), True)

    def test_errmsg_command(self):
        modified_lines = [(0, ""), (1, "class AddShardCmd : public ErrmsgCommandDeprecated")]
        self.assertEqual(check_file_for_legacy_type(modified_lines), True)

    def test_generic_command_inherited(self):
        # This might appear in non-legacy command types and should not be mistake for a Command type

        modified_lines = [(0, ""), (1, "class AddShardCmd : public Command")]
        self.assertEqual(check_file_for_legacy_type(modified_lines), False)

    def test_kCommand(self):
        # This log statement appears in many Command files for logging purposes and should not be
        # mistaken for a Command type

        modified_lines = [
            (0, ""),
            (1, "#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand"),
        ]
        self.assertEqual(check_file_for_legacy_type(modified_lines), False)

    def test_command_invocation(self):
        # This class is not a Command type and should not be mistaken for a Command type

        modified_lines = [(0, ""), (1, "class AddShardCmd : public CommandInvocation")]
        self.assertEqual(check_file_for_legacy_type(modified_lines), False)

    def test_command_invocation_hooks(self):
        # This class is not a Command type and should not be mistaken for a Command type

        modified_lines = [(0, ""), (1, "class AddShardCmd : public CommandInvocationHooks")]
        self.assertEqual(check_file_for_legacy_type(modified_lines), False)
