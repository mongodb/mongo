"""Unit tests for bazel.wrapper_hook.hermetic_container.symlinks."""

from __future__ import annotations

import importlib.util
import pathlib
import tempfile
import unittest

from bazel.wrapper_hook import hermetic_container_integration

HERMETIC_CONTAINER_PATH = (
    pathlib.Path(__file__).parents[2] / "hermetic_container" / "hermetic_container.py"
)
HERMETIC_CONTAINER_SPEC = importlib.util.spec_from_file_location(
    "hermetic_container_under_test", HERMETIC_CONTAINER_PATH
)
assert HERMETIC_CONTAINER_SPEC is not None
hermetic_container = importlib.util.module_from_spec(HERMETIC_CONTAINER_SPEC)
assert HERMETIC_CONTAINER_SPEC.loader is not None
HERMETIC_CONTAINER_SPEC.loader.exec_module(hermetic_container)


class HermeticContainerConvenienceSymlinkTest(unittest.TestCase):
    def test_publishes_shared_install_symlink_and_replaces_legacy_directory(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            repo_root = root / "repo"
            bin_dir = root / "output" / "execroot" / "_main" / "bazel-out" / "k8-opt" / "bin"
            shared_install_root = root / "shared-install"
            repo_root.mkdir()
            bin_dir.mkdir(parents=True)
            shared_install_root.mkdir()
            (repo_root / "bazel-bin").symlink_to(bin_dir, target_is_directory=True)
            legacy_install = bin_dir / "install"
            legacy_install.mkdir()
            (legacy_install / "old-binary").write_text("old", encoding="utf-8")

            hermetic_container_integration._publish_linux_shared_install_symlink(
                {"shared_install_dir": str(shared_install_root)},
                repo_root=repo_root,
            )

            self.assertTrue((repo_root / "bazel-bin").is_symlink())
            self.assertEqual(bin_dir.resolve(), (repo_root / "bazel-bin").resolve())
            install_link = repo_root / "bazel-bin" / "install"
            self.assertTrue(install_link.is_symlink())
            self.assertEqual((shared_install_root / "k8-opt").resolve(), install_link.resolve())

    def test_publishes_shared_install_symlink_for_relative_bazel_bin_target(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            repo_root = root / "repo"
            bin_dir = root / "output" / "execroot" / "_main" / "bazel-out" / "k8-opt" / "bin"
            shared_install_root = root / "shared-install"
            repo_root.mkdir()
            bin_dir.mkdir(parents=True)
            shared_install_root.mkdir()
            (repo_root / "bazel-bin").symlink_to(
                pathlib.Path("../output/execroot/_main/bazel-out/k8-opt/bin"),
                target_is_directory=True,
            )

            hermetic_container_integration._publish_linux_shared_install_symlink(
                {"shared_install_dir": str(shared_install_root)},
                repo_root=repo_root,
            )

            install_link = repo_root / "bazel-bin" / "install"
            self.assertTrue(install_link.is_symlink())
            self.assertEqual((shared_install_root / "k8-opt").resolve(), install_link.resolve())

    def test_publishes_shared_install_symlink_when_bazel_bin_is_a_directory(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            repo_root = root / "repo"
            shared_install_root = root / "shared-install"
            install_dir = shared_install_root / "ppc-opt" / "bin"
            repo_root.mkdir()
            (repo_root / "bazel-bin").mkdir()
            install_dir.mkdir(parents=True)
            (install_dir / "mongod").write_text("binary", encoding="utf-8")

            hermetic_container_integration._publish_linux_shared_install_symlink(
                {"shared_install_dir": str(shared_install_root)},
                repo_root=repo_root,
            )

            install_link = repo_root / "bazel-bin" / "install"
            self.assertTrue(install_link.is_symlink())
            self.assertEqual((shared_install_root / "ppc-opt").resolve(), install_link.resolve())

    def test_uses_private_install_tree_when_shared_tree_is_empty(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            repo_root = root / "repo"
            bin_dir = root / "output" / "execroot" / "_main" / "bazel-out" / "ppc-opt" / "bin"
            shared_install_root = root / "shared-install"
            private_install_dir = bin_dir / "install-dist-test" / "bin"
            repo_root.mkdir()
            bin_dir.mkdir(parents=True)
            shared_install_root.mkdir()
            private_install_dir.mkdir(parents=True)
            (private_install_dir / "mongod").write_text("binary", encoding="utf-8")
            (repo_root / "bazel-bin").symlink_to(bin_dir, target_is_directory=True)

            hermetic_container_integration._publish_linux_shared_install_symlink(
                {"shared_install_dir": str(shared_install_root)},
                repo_root=repo_root,
            )

            install_link = repo_root / "bazel-bin" / "install"
            self.assertTrue(install_link.is_symlink())
            self.assertEqual(private_install_dir.parent.resolve(), install_link.resolve())

    def test_uses_private_dbtest_install_tree_when_shared_tree_is_empty(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            repo_root = root / "repo"
            bin_dir = root / "output" / "execroot" / "_main" / "bazel-out" / "ppc-opt" / "bin"
            shared_install_root = root / "shared-install"
            private_install_dir = bin_dir / "install-dbtest" / "bin"
            repo_root.mkdir()
            bin_dir.mkdir(parents=True)
            shared_install_root.mkdir()
            private_install_dir.mkdir(parents=True)
            (private_install_dir / "dbtest").write_text("binary", encoding="utf-8")
            (repo_root / "bazel-bin").symlink_to(bin_dir, target_is_directory=True)

            hermetic_container_integration._publish_linux_shared_install_symlink(
                {"shared_install_dir": str(shared_install_root)},
                repo_root=repo_root,
            )

            install_link = repo_root / "bazel-bin" / "install"
            self.assertTrue(install_link.is_symlink())
            self.assertEqual(private_install_dir.parent.resolve(), install_link.resolve())

    def test_preferred_private_install_tree_wins_over_other_install_outputs(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            repo_root = root / "repo"
            bin_dir = root / "output" / "execroot" / "_main" / "bazel-out" / "ppc-opt" / "bin"
            shared_install_root = root / "shared-install"
            dbtest_install_dir = bin_dir / "install-dbtest" / "bin"
            dist_install_dir = bin_dir / "install-dist-test" / "bin"
            repo_root.mkdir()
            bin_dir.mkdir(parents=True)
            shared_install_root.mkdir()
            dbtest_install_dir.mkdir(parents=True)
            dist_install_dir.mkdir(parents=True)
            (dbtest_install_dir / "dbtest").write_text("binary", encoding="utf-8")
            (dist_install_dir / "mongod").write_text("binary", encoding="utf-8")
            (repo_root / "bazel-bin").symlink_to(bin_dir, target_is_directory=True)

            hermetic_container_integration._publish_linux_shared_install_symlink(
                {
                    "shared_install_dir": str(shared_install_root),
                    "preferred_install_target": "install-dbtest",
                },
                repo_root=repo_root,
            )

            install_link = repo_root / "bazel-bin" / "install"
            self.assertTrue(install_link.is_symlink())
            self.assertEqual(dbtest_install_dir.parent.resolve(), install_link.resolve())

    def test_uses_private_integration_install_tree_when_shared_tree_is_empty(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            repo_root = root / "repo"
            bin_dir = root / "output" / "execroot" / "_main" / "bazel-out" / "s390-opt" / "bin"
            shared_install_root = root / "shared-install"
            private_install_dir = bin_dir / "install-mongo_integration_test"
            repo_root.mkdir()
            bin_dir.mkdir(parents=True)
            shared_install_root.mkdir()
            (private_install_dir / "bin").mkdir(parents=True)
            (private_install_dir / "bin" / "mongod").write_text("binary", encoding="utf-8")
            (private_install_dir / "install-mongo_integration_test_test_list.txt").write_text(
                "test", encoding="utf-8"
            )
            (repo_root / "bazel-bin").symlink_to(bin_dir, target_is_directory=True)

            hermetic_container_integration._publish_linux_shared_install_symlink(
                {"shared_install_dir": str(shared_install_root)},
                repo_root=repo_root,
            )

            install_link = repo_root / "bazel-bin" / "install"
            self.assertTrue(install_link.is_symlink())
            self.assertEqual(private_install_dir.resolve(), install_link.resolve())

    def test_recovers_private_install_tree_when_bazel_bin_link_is_dangling(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            repo_root = root / "repo"
            output_base = root / "output"
            bin_dir = output_base / "execroot" / "_main" / "bazel-out" / "ppc-opt" / "bin"
            shared_install_root = root / "shared-install"
            private_install_dir = bin_dir / "install-dist-test" / "bin"
            repo_root.mkdir()
            bin_dir.mkdir(parents=True)
            shared_install_root.mkdir()
            private_install_dir.mkdir(parents=True)
            (private_install_dir / "mongod").write_text("binary", encoding="utf-8")
            (repo_root / "bazel-bin").symlink_to(
                root / "container-only" / "bazel-out" / "ppc-opt" / "bin",
                target_is_directory=True,
            )

            hermetic_container_integration._publish_linux_shared_install_symlink(
                {
                    "shared_install_dir": str(shared_install_root),
                    "output_base": str(output_base),
                },
                repo_root=repo_root,
            )

            self.assertTrue((repo_root / "bazel-bin").is_symlink())
            self.assertEqual(bin_dir.resolve(), (repo_root / "bazel-bin").resolve())
            install_link = repo_root / "bazel-bin" / "install"
            self.assertTrue(install_link.is_symlink())
            self.assertEqual(private_install_dir.parent.resolve(), install_link.resolve())

    def test_adds_standard_convenience_symlink_prefix(self):
        self.assertEqual(
            hermetic_container_integration._bazel_args_with_hermetic_container_symlink_prefix(
                ["--output_base=/tmp/output", "build", "//src/mongo:mongo"]
            ),
            [
                "--output_base=/tmp/output",
                "build",
                "--symlink_prefix=bazel-",
                "//src/mongo:mongo",
            ],
        )

    def test_preserves_explicit_convenience_symlink_prefix(self):
        args = ["build", "--symlink_prefix=custom/bazel-", "//src/mongo:mongo"]

        self.assertEqual(
            hermetic_container_integration._bazel_args_with_hermetic_container_symlink_prefix(args),
            args,
        )

    def test_temporary_hermetic_container_engflow_bazelrc_does_not_modify_host_contents(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir)
            bazelrc = repo_root / ".bazelrc.engflow_creds"
            host_contents = (
                "common --credential_helper=sodalite.cluster.engflow.com=C:/host/helper.exe\n"
            )
            bazelrc.write_text(host_contents, encoding="utf-8")

            with hermetic_container_integration._temporary_hermetic_container_engflow_bazelrc(
                "/container/engflow_auth",
                repo_root,
            ) as dedicated_bazelrc:
                self.assertIsNotNone(dedicated_bazelrc)
                dedicated_path = pathlib.Path(dedicated_bazelrc)
                self.assertNotEqual(dedicated_path, bazelrc)
                self.assertEqual(
                    dedicated_path.read_text(encoding="utf-8"),
                    (
                        "common --credential_helper="
                        "sodalite.cluster.engflow.com=/container/engflow_auth\n"
                    ),
                )
                self.assertEqual(bazelrc.read_text(encoding="utf-8"), host_contents)

            self.assertFalse(dedicated_path.exists())
            self.assertEqual(bazelrc.read_text(encoding="utf-8"), host_contents)

    def test_restores_hermetic_container_convenience_symlinks_from_marker(self):
        class FakeDockerInstance:
            workspace_hex_digest = "container-hash"
            bazel_output_base_digest = "output-base-hash"

        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            repo_root = root / "repo"
            repo_root.mkdir()
            output_root = root / "out"
            execroot = output_root / "output-base-hash" / "execroot" / "_main"
            bazel_out = execroot / "bazel-out"
            bin_dir = bazel_out / "aarch64-fastbuild" / "bin"
            testlogs_dir = bazel_out / "aarch64-fastbuild" / "testlogs"
            bin_dir.mkdir(parents=True)
            testlogs_dir.mkdir(parents=True)
            host_bin = root / "host-bin"
            host_bin.mkdir()
            convenience_dir = repo_root
            (convenience_dir / "bazel-bin").symlink_to(bin_dir, target_is_directory=True)
            (convenience_dir / "bazel-out").symlink_to(bazel_out, target_is_directory=True)
            (convenience_dir / "bazel-testlogs").symlink_to(testlogs_dir, target_is_directory=True)

            image = hermetic_container_integration.parse_docker_image(
                "quay.io/mongodb/rbe@sha256:abc123"
            )
            config = hermetic_container_integration.HermeticContainerConfig(
                distro="amazon_linux_2023",
                docker_image=image,
                instance_name="mongo_hermetic_container_test",
                bazel_real="/tmp/bazel-darwin",
                bazel_command="/tmp/bazel-linux",
                bazel_user_output_root=str(output_root),
                hermetic_container_run_file=str(root / "container.run"),
                user="1:1",
                volumes=[],
                env_vars=[],
                platform="",
                privileged=False,
            )
            marker = root / "symlinks.json"

            hermetic_container_integration._publish_hermetic_container_convenience_symlinks(
                config,
                FakeDockerInstance(),
                {
                    hermetic_container_integration.HERMETIC_CONTAINER_CONVENIENCE_SYMLINKS_ENV: str(
                        marker
                    )
                },
                repo_root=repo_root,
            )
            (repo_root / "bazel-bin").unlink()
            (repo_root / "bazel-bin").symlink_to(host_bin, target_is_directory=True)

            hermetic_container_integration.restore_hermetic_container_convenience_symlinks_from_env(
                {
                    hermetic_container_integration.HERMETIC_CONTAINER_CONVENIENCE_SYMLINKS_ENV: str(
                        marker
                    )
                }
            )

            self.assertEqual((repo_root / "bazel-bin").resolve(), bin_dir.resolve())
            self.assertEqual((repo_root / "bazel-out").resolve(), bazel_out.resolve())
            self.assertEqual((repo_root / "bazel-testlogs").resolve(), testlogs_dir.resolve())
            self.assertEqual((repo_root / "bazel-repo").resolve(), execroot.resolve())


if __name__ == "__main__":
    unittest.main()
