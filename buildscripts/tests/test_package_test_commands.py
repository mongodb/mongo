import unittest

from buildscripts.package_test import package_test_commands as under_test


class PackageTestCommandsTest(unittest.TestCase):
    def test_apt_commands_retry_transient_download_failures(self) -> None:
        apt_commands = under_test.PACKAGE_MANAGER_COMMANDS["apt"]

        self.assertIn(
            f"apt-get -o Acquire::Retries={under_test.APT_RETRIES}", apt_commands["update"]
        )
        self.assertIn(
            f"apt-get -o Acquire::Retries={under_test.APT_RETRIES}", apt_commands["install"]
        )


if __name__ == "__main__":
    unittest.main()
