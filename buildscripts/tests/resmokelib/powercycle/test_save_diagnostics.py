"""Unit tests for buildscripts.resmokelib.powercycle.save_diagnostics."""

import unittest
from unittest.mock import MagicMock, patch

from buildscripts.resmokelib.powercycle.save_diagnostics import (
    CopyEC2MonitorFiles,
    TarEC2Artifacts,
)


def _build(command_cls, expansions):
    """Construct a powercycle command without running its usual __init__ setup."""
    command = object.__new__(command_cls)
    command.expansions = expansions
    command.remote_op = MagicMock()
    return command


class TestTarToleratesMissingArtifacts(unittest.TestCase):
    """The remote tar must not fail the post block when artifacts were never produced.

    A powercycle run that never started mongod has no logs, and the backup globs stay
    unexpanded when nothing matches, so tar is routinely handed paths that do not exist.
    """

    @patch(
        "buildscripts.resmokelib.powercycle.save_diagnostics.PowercycleCommand.is_windows",
        return_value=False,
    )
    def test_tar_ec2_artifacts_ignores_missing_files(self, _mock_is_windows):
        command = _build(TarEC2Artifacts, {"exit_code": "1"})

        command.execute()

        cmd = command.remote_op.operation.call_args[0][1]
        self.assertIn("--ignore-failed-read", cmd)

    @patch(
        "buildscripts.resmokelib.powercycle.save_diagnostics.PowercycleCommand.is_windows",
        return_value=False,
    )
    def test_tar_ec2_artifacts_on_success_ignores_missing_files(self, _mock_is_windows):
        # On success only mongod.log is archived, and it too can be absent.
        command = _build(TarEC2Artifacts, {"exit_code": "0"})

        command.execute()

        cmd = command.remote_op.operation.call_args[0][1]
        self.assertIn("--ignore-failed-read", cmd)

    def test_tar_ec2_artifacts_skipped_on_ssh_failure(self):
        command = _build(TarEC2Artifacts, {"ec2_ssh_failure": "yes"})

        command.execute()

        command.remote_op.operation.assert_not_called()

    def test_copy_ec2_monitor_files_ignores_missing_files(self):
        command = _build(CopyEC2MonitorFiles, {})

        command.execute()

        cmd = command.remote_op.operation.call_args_list[0][0][1]
        self.assertIn("--ignore-failed-read", cmd)


if __name__ == "__main__":
    unittest.main()
