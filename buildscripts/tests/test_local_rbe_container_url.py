from __future__ import annotations

import pathlib
import tempfile
import unittest
from unittest import mock

from buildscripts import local_rbe_container_url


class LocalRbeContainerUrlTest(unittest.TestCase):
    def test_detects_fixed_amazon_linux_2023_3_release(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            os_release = root / "os-release"
            system_release = root / "system-release"
            os_release.write_text(
                'NAME="Amazon Linux"\nVERSION_ID="2023"\n',
                encoding="utf-8",
            )
            system_release.write_text(
                "Amazon Linux release 2023.3.20240312 (Amazon Linux)\n",
                encoding="utf-8",
            )

            with mock.patch(
                "subprocess.run",
                side_effect=AssertionError("os-release parsing must not invoke external tools"),
            ):
                self.assertEqual(
                    local_rbe_container_url.get_host_distro_major_version(
                        os_release,
                        system_release,
                        platform="linux",
                    ),
                    "amazon_linux_2023_3",
                )

    def test_keeps_rolling_amazon_linux_2023_release(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            os_release = root / "os-release"
            system_release = root / "system-release"
            os_release.write_text(
                'NAME="Amazon Linux"\nVERSION_ID="2023"\n',
                encoding="utf-8",
            )
            system_release.write_text(
                "Amazon Linux release 2023.7.20250609 (Amazon Linux)\n",
                encoding="utf-8",
            )

            self.assertEqual(
                local_rbe_container_url.get_host_distro_major_version(
                    os_release,
                    system_release,
                    platform="linux",
                ),
                "amazon_linux_2023",
            )

    def test_non_linux_hosts_do_not_inspect_linux_release_files(self) -> None:
        missing_path = pathlib.Path("/path/that/does/not/exist")

        self.assertEqual(
            local_rbe_container_url.get_host_distro_major_version(
                missing_path,
                missing_path,
                platform="darwin",
            ),
            "UNKNOWN",
        )


if __name__ == "__main__":
    unittest.main()
