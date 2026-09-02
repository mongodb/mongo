import unittest

from buildscripts.package_test import package_test_commands as under_test


class PackageTestCommandsTest(unittest.TestCase):
    def test_python_setup_validates_native_python_without_symlinking(self) -> None:
        commands = under_test.build_python_setup_commands("python3")

        self.assertEqual(1, len(commands))
        self.assertIn("python3 --version", commands[0])
        self.assertIn("Required Python interpreter python3 is unavailable", commands[0])
        self.assertNotIn("ln -s", commands[0])

    def test_python_setup_validates_scl_python_before_symlinking(self) -> None:
        python_command = "/opt/rh/rh-python38/root/usr/bin/python3"
        commands = under_test.build_python_setup_commands(python_command)

        self.assertEqual(2, len(commands))
        self.assertIn(f"{python_command} --version", commands[0])
        self.assertIn("Required Python interpreter", commands[0])
        self.assertEqual(f"ln -s {python_command} /usr/bin/python3", commands[1])

    def test_apt_commands_retry_transient_download_failures(self) -> None:
        apt_commands = under_test.PACKAGE_MANAGER_COMMANDS["apt"]

        self.assertIn(
            f"apt-get -o Acquire::Retries={under_test.APT_RETRIES}", apt_commands["update"]
        )
        self.assertIn(
            f"apt-get -o Acquire::Retries={under_test.APT_RETRIES}", apt_commands["install"]
        )

    def test_release_package_test_args_include_system_library_skip(self):
        self.assertEqual(
            [
                "--edition",
                "enterprise",
                "--platform",
                "ubuntu2404",
                "--skip-system-library-check",
                "package.tgz",
            ],
            under_test.build_package_test_internal_args(
                "enterprise", "ubuntu2404", ["package.tgz"], skip_system_library_check=True
            ),
        )

    def test_branch_package_test_args_do_not_include_system_library_skip(self):
        args = under_test.build_package_test_internal_args(
            "enterprise", "ubuntu2404", ["package.tgz"]
        )

        self.assertNotIn("--skip-system-library-check", args)


if __name__ == "__main__":
    unittest.main()
