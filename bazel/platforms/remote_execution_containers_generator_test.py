from __future__ import annotations

import hashlib
import importlib.util
import io
import json
import os
import pathlib
import subprocess
import sys
import tarfile
import tempfile
import types
import unittest
from unittest import mock


def _load_generator() -> types.ModuleType:
    retry_module = types.ModuleType("retry")
    retry_module.retry = lambda **_kwargs: lambda function: function
    path = pathlib.Path(__file__).with_name("remote_execution_containers_generator.py")
    spec = importlib.util.spec_from_file_location("remote_execution_containers_generator", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Could not load {path}")
    module = importlib.util.module_from_spec(spec)
    with mock.patch.dict(sys.modules, {"retry": retry_module}):
        spec.loader.exec_module(module)
    return module


generator = _load_generator()


class SysrootArchiveTest(unittest.TestCase):
    @staticmethod
    def _rootfs(path: pathlib.Path, contents: bytes = b"header", mtime: int = 0) -> None:
        with tarfile.open(path, "w") as archive:
            for name, data in (
                ("usr/include/example.h", contents),
                ("usr/lib64/libc.so", b"library"),
                ("usr/bin/compiler", b"excluded"),
                ("etc/ssl/private/key", b"excluded"),
            ):
                member = tarfile.TarInfo(name)
                member.size = len(data)
                member.mtime = mtime
                archive.addfile(member, io.BytesIO(data))
            for name, target in (
                ("lib64", "/usr/lib64"),
                ("usr/lib64/libc-link.so", "/lib64/libc.so"),
                ("usr/lib64/ssl", "/etc/ssl"),
            ):
                member = tarfile.TarInfo(name)
                member.type = tarfile.SYMTYPE
                member.linkname = target
                archive.addfile(member)

    def test_archive_contains_only_sysroot_paths_and_relative_links(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            directory = pathlib.Path(temp_dir)
            rootfs = directory / "rootfs.tar"
            self._rootfs(rootfs)
            archive = directory / "sysroot.tar.gz"

            generator.create_sysroot_archive(rootfs, archive)

            extracted = directory / "extracted"
            with tarfile.open(archive) as packaged:
                packaged.extractall(extracted, filter="data")
            self.assertEqual(b"header", (extracted / "usr/include/example.h").read_bytes())
            self.assertEqual(b"library", (extracted / "lib64/libc-link.so").read_bytes())
            self.assertEqual("usr/lib64", (extracted / "lib64").readlink().as_posix())
            self.assertFalse((extracted / "usr/bin").exists())
            self.assertFalse((extracted / "etc").exists())
            self.assertFalse((extracted / "usr/lib64/ssl").is_symlink())

    def test_archive_hash_is_stable_and_changes_with_contents(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            directory = pathlib.Path(temp_dir)
            rootfs = directory / "rootfs.tar"
            self._rootfs(rootfs)
            first = directory / "first.tar.gz"
            second = directory / "second.tar.gz"
            first_hash = generator.create_sysroot_archive(rootfs, first)
            self._rootfs(rootfs, mtime=123456789)
            self.assertEqual(first_hash, generator.create_sysroot_archive(rootfs, second))
            self.assertEqual(first.read_bytes(), second.read_bytes())
            self.assertEqual(hashlib.sha256(first.read_bytes()).hexdigest(), first_hash)

            self._rootfs(rootfs, b"changed header")
            self.assertNotEqual(first_hash, generator.create_sysroot_archive(rootfs, second))

    def test_archive_hash_and_directory_modes_do_not_depend_on_umask(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            directory = pathlib.Path(temp_dir)
            rootfs = directory / "rootfs.tar"
            self._rootfs(rootfs)
            hashes = []
            archives = []
            for umask in (0o022, 0o077):
                archive = directory / f"sysroot-{umask}.tar.gz"
                original_umask = os.umask(umask)
                try:
                    hashes.append(generator.create_sysroot_archive(rootfs, archive))
                finally:
                    os.umask(original_umask)
                archives.append(archive.read_bytes())
                with tarfile.open(archive) as packaged:
                    directories = {
                        member.name: member.mode for member in packaged if member.isdir()
                    }
                self.assertIn(
                    "usr", directories, "the implicitly created parent must be normalized"
                )
                self.assertEqual({0o755}, set(directories.values()))
            self.assertEqual(hashes[0], hashes[1])
            self.assertEqual(archives[0], archives[1])

    def test_malformed_rootfs_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            directory = pathlib.Path(temp_dir)
            rootfs = directory / "rootfs.tar"
            with tarfile.open(rootfs, "w"):
                pass
            with self.assertRaisesRegex(ValueError, "usr/include"):
                generator.create_sysroot_archive(rootfs, directory / "sysroot.tar.gz")

    def test_hard_linked_headers_are_preserved(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            directory = pathlib.Path(temp_dir)
            rootfs = directory / "rootfs.tar"
            self._rootfs(rootfs)
            with tarfile.open(rootfs, "a") as exported:
                member = tarfile.TarInfo("usr/include/linked.h")
                member.type = tarfile.LNKTYPE
                member.linkname = "usr/include/example.h"
                exported.addfile(member)
            archive = directory / "sysroot.tar.gz"
            generator.create_sysroot_archive(rootfs, archive)
            with tarfile.open(archive) as packaged:
                packaged.extractall(directory / "extracted", filter="data")
            self.assertEqual(b"header", (directory / "extracted/usr/include/linked.h").read_bytes())

    def test_library_links_through_system_alternatives_are_resolved(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            self._rootfs(root / "rootfs.tar")
            with tarfile.open(root / "rootfs.tar", "a") as archive:
                for name, target in (
                    ("usr/lib64/selected.so", "/etc/alternatives/selected.so"),
                    ("etc/alternatives/selected.so", "/usr/lib64/libc.so"),
                ):
                    member = tarfile.TarInfo(name)
                    member.type, member.linkname = tarfile.SYMTYPE, target
                    archive.addfile(member)
            generator.create_sysroot_archive(root / "rootfs.tar", root / "sysroot.tar.gz")
            with tarfile.open(root / "sysroot.tar.gz") as archive:
                archive.extractall(root / "extracted", filter="data")
            self.assertEqual(b"library", (root / "extracted/usr/lib64/selected.so").read_bytes())
            self.assertFalse((root / "extracted/etc").exists())

    def test_cyclic_system_alternatives_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            self._rootfs(root / "rootfs.tar")
            with tarfile.open(root / "rootfs.tar", "a") as archive:
                for name, target in (
                    ("usr/lib64/selected.so", "/etc/alternatives/one"),
                    ("etc/alternatives/one", "two"),
                    ("etc/alternatives/two", "one"),
                ):
                    member = tarfile.TarInfo(name)
                    member.type, member.linkname = tarfile.SYMTYPE, target
                    archive.addfile(member)
            with self.assertRaisesRegex(ValueError, "cyclic external"):
                generator.create_sysroot_archive(root / "rootfs.tar", root / "sysroot.tar.gz")


class ToolchainSysrootArchiveTest(unittest.TestCase):
    @staticmethod
    def _rootfs(path: pathlib.Path, architecture: str, *, omitted: str = "") -> None:
        machine, byteorder, loader = {
            "amd64": (62, "little", "ld-linux-x86-64.so.2"),
            "arm64": (183, "little", "ld-linux-aarch64.so.1"),
            "ppc64le": (21, "little", "ld64.so.2"),
            "s390x": (22, "big", "ld64.so.1"),
        }[architecture]
        elf = bytearray(64)
        elf[:6] = b"\x7fELF\x02" + bytes([1 if byteorder == "little" else 2])
        elf[18:20] = machine.to_bytes(2, byteorder)
        with tarfile.open(path, "w") as archive:
            files = {
                **{
                    f"usr/include/{name}": b"header"
                    for name in (
                        "stdio.h",
                        "linux/version.h",
                        "asm/unistd.h",
                        "sys/sdt.h",
                        "crypt.h",
                    )
                },
                **{
                    f"usr/lib64/{name}": bytes(elf)
                    for name in (
                        "crt1.o",
                        "crti.o",
                        "crtn.o",
                        "libc.so.6",
                        "libgcc_s.so.1",
                        "libcrypt.so",
                        loader,
                    )
                },
                "usr/lib64/libc.so": b"GROUP ( libc.so.6 libc_nonshared.a )",
                "usr/lib64/libc_nonshared.a": b"!<arch>\n",
            }
            for name, data in files.items():
                if name == omitted:
                    continue
                member = tarfile.TarInfo(name)
                member.size = len(data)
                archive.addfile(member, io.BytesIO(data))

    @staticmethod
    def _provenance(architecture: str = "s390x") -> dict:
        return {
            "schema-version": 1,
            "distro": "rhel8",
            "architecture": architecture,
            "image": "repo@sha256:target",
            "packages": ["glibc-2.28.s390x", "systemtap-sdt-devel.s390x"],
        }

    def test_shared_archive_records_provenance_and_validates_each_target(self) -> None:
        for architecture in ("amd64", "arm64", "ppc64le", "s390x"):
            with self.subTest(architecture=architecture), tempfile.TemporaryDirectory() as tmp:
                root = pathlib.Path(tmp)
                rootfs, archive = root / "rootfs.tar", root / "sysroot.tar.gz"
                self._rootfs(rootfs, architecture)
                generator.create_sysroot_archive(
                    rootfs, archive, provenance=self._provenance(architecture)
                )
                with tarfile.open(archive) as packaged:
                    manifest = json.load(packaged.extractfile("sysroot-manifest.json"))
                    self.assertEqual(architecture, manifest["architecture"])
                    self.assertEqual("repo@sha256:target", manifest["image"])
                    self.assertIn("systemtap-sdt-devel.s390x", manifest["packages"])
                    self.assertIn("usr/include/sys/sdt.h", manifest["validated-files"])

    def test_missing_exception_probe_header_prevents_publication(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            self._rootfs(root / "rootfs.tar", "s390x", omitted="usr/include/sys/sdt.h")
            with self.assertRaisesRegex(ValueError, "sys/sdt.h"):
                generator.create_sysroot_archive(
                    root / "rootfs.tar", root / "sysroot.tar.gz", provenance=self._provenance()
                )
            self.assertFalse((root / "sysroot.tar.gz").exists())

    def test_wrong_architecture_is_rejected_before_archiving(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            self._rootfs(root / "rootfs.tar", "arm64")
            with self.assertRaisesRegex(ValueError, "ELF architecture"):
                generator.create_sysroot_archive(
                    root / "rootfs.tar", root / "sysroot.tar.gz", provenance=self._provenance()
                )

    def test_incomplete_crt_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            self._rootfs(root / "rootfs.tar", "s390x", omitted="usr/lib64/crti.o")
            with self.assertRaisesRegex(ValueError, "crti.o"):
                generator.create_sysroot_archive(
                    root / "rootfs.tar", root / "sysroot.tar.gz", provenance=self._provenance()
                )

    def test_misplaced_runtime_file_cannot_replace_required_library_path(self) -> None:
        for name in ("crti.o", "libc.so", "libc_nonshared.a", "ld64.so.1"):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as tmp:
                root = pathlib.Path(tmp)
                self._rootfs(root / "rootfs.tar", "s390x")
                with tarfile.open(root / "rootfs.tar") as exported:
                    exported.extractall(root / "sysroot", filter="data")
                required = root / "sysroot/usr/lib64" / name
                misplaced = root / "sysroot/usr/lib/unrelated" / name
                misplaced.parent.mkdir(parents=True, exist_ok=True)
                required.rename(misplaced)
                with self.assertRaisesRegex(ValueError, f"usr/lib64/{name}"):
                    generator.validate_toolchain_sysroot(root / "sysroot", "s390x")

    def test_valid_duplicate_cannot_mask_wrong_architecture_at_required_path(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            self._rootfs(root / "rootfs.tar", "s390x")
            with tarfile.open(root / "rootfs.tar") as exported:
                data = exported.extractfile("usr/lib64/libc.so.6").read()
            with tarfile.open(root / "rootfs.tar", "a") as exported:
                decoy = tarfile.TarInfo("usr/lib/libc.so.6")
                decoy.size = len(data)
                exported.addfile(decoy, io.BytesIO(data))
                wrong = bytearray(data)
                wrong[18:20] = (62).to_bytes(2, "big")
                required = tarfile.TarInfo("usr/lib64/libc.so.6")
                required.size = len(wrong)
                exported.addfile(required, io.BytesIO(wrong))
            with self.assertRaisesRegex(ValueError, "ELF architecture.*usr/lib64/libc.so.6"):
                generator.create_sysroot_archive(
                    root / "rootfs.tar", root / "sysroot.tar.gz", provenance=self._provenance()
                )
            self.assertFalse((root / "sysroot.tar.gz").exists())

    def test_unrelated_32_bit_library_copy_does_not_replace_64_bit_validation(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            self._rootfs(root / "rootfs.tar", "s390x")
            with tarfile.open(root / "rootfs.tar", "a") as exported:
                data = b"\x7fELF\x01" + bytes(59)
                member = tarfile.TarInfo("usr/lib/libc.so.6")
                member.size = len(data)
                exported.addfile(member, io.BytesIO(data))
            generator.create_sysroot_archive(
                root / "rootfs.tar", root / "sysroot.tar.gz", provenance=self._provenance()
            )
            with tarfile.open(root / "sysroot.tar.gz") as packaged:
                manifest = json.load(packaged.extractfile("sysroot-manifest.json"))
                self.assertIn("usr/lib64/libc.so.6", manifest["validated-files"])
                self.assertNotIn("usr/lib/libc.so.6", manifest["validated-files"])

    def test_runtime_directory_aliases_and_loader_links_are_validated(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            self._rootfs(root / "rootfs.tar", "s390x")
            with tarfile.open(root / "rootfs.tar", "a") as exported:
                for name, target in (
                    ("lib", "usr/lib"),
                    ("lib64", "usr/lib64"),
                    ("usr/lib/ld64.so.1", "../lib64/ld64.so.1"),
                ):
                    member = tarfile.TarInfo(name)
                    member.type, member.linkname = tarfile.SYMTYPE, target
                    exported.addfile(member)
            generator.create_sysroot_archive(
                root / "rootfs.tar", root / "sysroot.tar.gz", provenance=self._provenance()
            )
            with tarfile.open(root / "sysroot.tar.gz") as packaged:
                manifest = json.load(packaged.extractfile("sysroot-manifest.json"))
                self.assertIn("usr/lib64/libc.so.6", manifest["validated-files"])
                self.assertIn("usr/lib/ld64.so.1", manifest["validated-files"])

    def test_wrong_architecture_runtime_alias_is_rejected(self) -> None:
        for name in ("lib64/libc.so.6", "usr/lib/ld64.so.1"):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as tmp:
                root = pathlib.Path(tmp)
                self._rootfs(root / "rootfs.tar", "s390x")
                with tarfile.open(root / "rootfs.tar") as exported:
                    data = bytearray(exported.extractfile("usr/lib64/libc.so.6").read())
                data[18:20] = (62).to_bytes(2, "big")
                with tarfile.open(root / "rootfs.tar", "a") as exported:
                    member = tarfile.TarInfo(name)
                    member.size = len(data)
                    exported.addfile(member, io.BytesIO(data))
                with self.assertRaisesRegex(ValueError, f"ELF architecture.*{name}"):
                    generator.create_sysroot_archive(
                        root / "rootfs.tar", root / "sysroot.tar.gz", provenance=self._provenance()
                    )

    def test_dangling_library_link_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            self._rootfs(root / "rootfs.tar", "s390x")
            with tarfile.open(root / "rootfs.tar", "a") as archive:
                member = tarfile.TarInfo("usr/lib64/broken.so")
                member.type, member.linkname = tarfile.SYMTYPE, "missing.so"
                archive.addfile(member)
            with self.assertRaisesRegex(ValueError, "dangling"):
                generator.create_sysroot_archive(
                    root / "rootfs.tar", root / "sysroot.tar.gz", provenance=self._provenance()
                )

    def test_build_id_indexes_for_excluded_executables_are_omitted(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            self._rootfs(root / "rootfs.tar", "s390x")
            with tarfile.open(root / "rootfs.tar", "a") as archive:
                for library_dir in ("usr/lib", "usr/lib64"):
                    member = tarfile.TarInfo(f"{library_dir}/.build-id/ab/cdef")
                    member.type, member.linkname = tarfile.SYMTYPE, "../../excluded-executable"
                    archive.addfile(member)
            generator.create_sysroot_archive(
                root / "rootfs.tar", root / "sysroot.tar.gz", provenance=self._provenance()
            )
            with tarfile.open(root / "sysroot.tar.gz") as archive:
                self.assertFalse(any(".build-id" in name for name in archive.getnames()))

    def test_provenance_archive_hash_is_independent_of_umask(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            self._rootfs(root / "rootfs.tar", "s390x")
            hashes = []
            for umask in (0o022, 0o077):
                previous = os.umask(umask)
                try:
                    hashes.append(
                        generator.create_sysroot_archive(
                            root / "rootfs.tar",
                            root / "sysroot.tar.gz",
                            provenance=self._provenance(),
                        )
                    )
                finally:
                    os.umask(previous)
            self.assertEqual(hashes[0], hashes[1])


class SysrootPublicationTest(unittest.TestCase):
    def _publish(
        self, distro: str, fail_upload: bool = False, dry_run: bool = False
    ) -> tuple[dict[str, str], mock.Mock]:
        manifests = [
            {
                "platform": {"os": "unknown", "architecture": "unknown"},
                "digest": "sha256:attestation",
            },
            *[
                {"platform": {"os": "linux", "architecture": arch}, "digest": f"sha256:{arch}"}
                for arch in generator.PUBLISHED_ARCHITECTURES
            ],
        ]

        def fake_run(command: list[str], **kwargs):
            if command[:4] == ["docker", "buildx", "imagetools", "inspect"]:
                return mock.Mock(stdout=json.dumps({"manifests": manifests}))
            if command[:2] == ["docker", "create"]:
                architecture = command[3].split("/")[1]
                return mock.Mock(stdout=f"container-{architecture}\n")
            if command[:2] == ["docker", "export"]:
                SysrootArchiveTest._rootfs(pathlib.Path(command[3]), command[4].encode())
            if command[:3] == ["aws", "s3", "cp"]:
                if fail_upload:
                    raise subprocess.CalledProcessError(1, command)
                sha256 = hashlib.sha256(pathlib.Path(command[3]).read_bytes()).hexdigest()
                self.assertTrue(command[4].endswith(f"-{sha256}.tar.gz"))
                self.assertTrue(kwargs["check"])
            return mock.Mock()

        with mock.patch.object(generator, "log_subprocess_run", side_effect=fake_run) as run:
            if fail_upload:
                with self.assertRaises(subprocess.CalledProcessError):
                    generator.publish_sysroots("repo@sha256:index", distro, "bucket", "sysroots")
                return {}, run
            return generator.publish_sysroots(
                "repo@sha256:index", distro, "bucket", "sysroots", dry_run=dry_run
            ), run

    def test_dry_run_exports_and_hashes_every_architecture_without_using_s3_credentials(
        self,
    ) -> None:
        with mock.patch.object(
            generator,
            "sysroot_upload_environment",
            side_effect=AssertionError("S3 credentials used"),
        ):
            metadata, run = self._publish("rhel9", dry_run=True)
        commands = [call.args[0] for call in run.call_args_list]
        self.assertFalse(any(command[0] == "aws" for command in commands))
        for architecture in generator.PUBLISHED_ARCHITECTURES:
            self.assertIn(f"sysroot-{architecture}-url", metadata)
            self.assertEqual(64, len(metadata[f"sysroot-{architecture}-sha256"]))
            self.assertTrue(
                any(
                    command[:2] == ["docker", "export"]
                    and command[-1] == f"container-{architecture}"
                    for command in commands
                )
            )
            self.assertIn(["docker", "rm", "--force", f"container-{architecture}"], commands)

    def test_rhel_exports_each_child_manifest_and_publishes_its_hash(self) -> None:
        metadata, run = self._publish("rhel9")
        commands = [call.args[0] for call in run.call_args_list]
        for architecture in generator.PUBLISHED_ARCHITECTURES:
            build_platform = (
                "linux/arm64/v8" if architecture == "arm64" else f"linux/{architecture}"
            )
            self.assertIn(
                [
                    "docker",
                    "create",
                    "--platform",
                    build_platform,
                    f"repo@sha256:{architecture}",
                    "/bin/true",
                ],
                commands,
            )
            sha256 = metadata[f"sysroot-{architecture}-sha256"]
            self.assertEqual(
                f"https://bucket.s3.amazonaws.com/sysroots/rbe-sysroot-rhel9-{architecture}-{sha256}.tar.gz",
                metadata[f"sysroot-{architecture}-url"],
            )
            self.assertIn(["docker", "rm", "--force", f"container-{architecture}"], commands)
        self.assertFalse(any(command[:2] == ["docker", "run"] for command in commands))

    def test_other_distros_export_only_amd64_and_arm64(self) -> None:
        metadata, _ = self._publish("ubuntu24")
        self.assertEqual(
            {
                "sysroot-amd64-url",
                "sysroot-amd64-sha256",
                "sysroot-arm64-url",
                "sysroot-arm64-sha256",
            },
            metadata.keys(),
        )

    def test_upload_failure_still_removes_the_container_and_image(self) -> None:
        _, run = self._publish("rhel9", fail_upload=True)
        commands = [call.args[0] for call in run.call_args_list]
        self.assertIn(["docker", "rm", "--force", "container-amd64"], commands)
        self.assertIn(["docker", "image", "rm", "repo@sha256:amd64"], commands)

    def test_missing_architecture_is_an_error(self) -> None:
        with mock.patch.object(
            generator, "log_subprocess_run", return_value=mock.Mock(stdout='{"manifests": []}')
        ):
            with self.assertRaisesRegex(ValueError, "no linux/amd64 manifest"):
                generator.publish_sysroots("repo@sha256:index", "rhel9", "bucket", "sysroots")

    def test_s3_credentials_do_not_use_the_ecr_session_token(self) -> None:
        with mock.patch.dict(
            generator.os.environ,
            {
                "aws_key_build": "build-key",
                "aws_secret_build": "build-secret",
                "AWS_ACCESS_KEY_ID": "ecr-key",
                "AWS_SECRET_ACCESS_KEY": "ecr-secret",
                "AWS_SESSION_TOKEN": "ecr-token",
            },
            clear=True,
        ):
            environment = generator.sysroot_upload_environment()
            self.assertEqual("build-key", environment["AWS_ACCESS_KEY_ID"])
            self.assertEqual("build-secret", environment["AWS_SECRET_ACCESS_KEY"])
            self.assertNotIn("AWS_SESSION_TOKEN", environment)
            self.assertEqual("ecr-token", generator.os.environ["AWS_SESSION_TOKEN"])

    def test_manual_upload_uses_the_default_aws_credentials(self) -> None:
        with mock.patch.dict(generator.os.environ, {"AWS_PROFILE": "publisher"}, clear=True):
            self.assertEqual({"AWS_PROFILE": "publisher"}, generator.sysroot_upload_environment())


class RemoteExecutionContainersGeneratorTest(unittest.TestCase):
    def test_rhel_images_build_all_supported_host_architectures(self) -> None:
        expected = (
            "linux/amd64",
            "linux/arm64/v8",
            "linux/ppc64le",
            "linux/s390x",
        )
        for distro in ("rhel8", "rhel9", "rhel10"):
            with self.subTest(distro=distro):
                self.assertEqual(expected, generator.platforms_for_distro(distro))

    def test_other_distros_keep_default_platforms(self) -> None:
        self.assertEqual(
            ("linux/amd64", "linux/arm64/v8"),
            generator.platforms_for_distro("debian12"),
        )

    def test_emulated_architectures_excludes_aarch64_host(self) -> None:
        self.assertEqual(
            ("amd64", "ppc64le", "s390x"),
            generator.emulated_architectures("aarch64"),
        )

    def test_emulated_architectures_excludes_x86_64_host(self) -> None:
        self.assertEqual(
            ("arm64", "ppc64le", "s390x"),
            generator.emulated_architectures("x86_64"),
        )

    def test_resolve_dockerfile_accepts_generated_lowercase_name(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            context = pathlib.Path(temp_dir)
            generated = context / "dockerfile"
            generated.touch()

            self.assertEqual(generated, generator.resolve_dockerfile(context / "Dockerfile"))

    def test_web_url_points_at_the_ecr_console_for_the_pushed_digest(self) -> None:
        digest = "sha256:" + "a" * 64
        self.assertEqual(
            f"https://gallery.ecr.aws/w3i8j1a8/devprod-build/{digest}",
            generator.web_url_for_image(f"public.ecr.aws/w3i8j1a8/devprod-build@{digest}"),
        )

    def test_create_buildx_builder_removes_stale_builder_before_each_attempt(self) -> None:
        with mock.patch.object(generator, "_log_subprocess_run") as run:
            generator.create_buildx_builder("test-builder")

        self.assertEqual(
            [
                mock.call(
                    ["docker", "buildx", "rm", "--force", "test-builder"],
                    check=False,
                ),
                mock.call(
                    [
                        "docker",
                        "buildx",
                        "create",
                        "--name",
                        "test-builder",
                        "--driver",
                        "docker-container",
                        "--use",
                        "--bootstrap",
                    ],
                    check=True,
                ),
            ],
            run.call_args_list,
        )


class EvergreenSysrootDryRunTest(unittest.TestCase):
    def test_patch_runs_prepare_sysroots_without_uploading_them(self) -> None:
        source = (
            pathlib.Path(__file__).parents[2] / "evergreen/remote_execution_containers_generate.sh"
        )
        for is_patch in ("true", "false", None):
            with self.subTest(is_patch=is_patch), tempfile.TemporaryDirectory() as temp_dir:
                workspace = pathlib.Path(temp_dir)
                evergreen = workspace / "src/evergreen"
                evergreen.mkdir(parents=True)
                script = evergreen / source.name
                script.write_text(source.read_text())
                # Exercise the real script with external build/publication commands replaced.
                (evergreen / "prelude.sh").write_text(
                    """\
sudo() { :; }
docker() { :; }
aws() { :; }
git() { :; }
bazel() { :; }
activate_venv() { :; }
capture_python() { printf '%s\\n' "$@" > "$CAPTURE_ARGS"; }
python=capture_python
"""
                )
                repin = workspace / "src/bazel/remote_execution_container/repin_dockerfiles.sh"
                repin.parent.mkdir(parents=True)
                repin.touch()
                captured = workspace / "generator-args"
                environment = {
                    "PATH": os.environ["PATH"],
                    "CAPTURE_ARGS": str(captured),
                    "remote_execution_container_distro": "rhel9",
                }
                if is_patch is not None:
                    environment["is_patch"] = is_patch
                subprocess.run(
                    ["bash", str(script)],
                    cwd=workspace,
                    env=environment,
                    capture_output=True,
                    check=True,
                )
                args = captured.read_text().splitlines()
                self.assertIn("--sysroot-s3-bucket=mdb-build-public", args)
                self.assertIn("--distro=rhel9", args)
                self.assertEqual(is_patch == "true", "--sysroot-dry-run" in args)


class ContinueOnErrorTest(unittest.TestCase):
    """--continue-on-error keeps a broken distro from discarding the distros behind it."""

    def _run_main(
        self,
        argv: list[str],
        failing_distro: str = "",
        failing_upload_distro: str = "",
        containers: dict | None = None,
    ) -> tuple[int, dict, list[str]]:
        containers = containers or {
            "debian12": {"dockerfile": "debian12/Dockerfile"},
            "ubuntu24": {"dockerfile": "ubuntu24/Dockerfile"},
        }
        built = []

        def publish_sysroots(_image, distro, _bucket, _prefix, *, dry_run=False):
            self.assertEqual("--sysroot-dry-run" in argv, dry_run)
            if distro == failing_upload_distro:
                raise subprocess.CalledProcessError(1, ["aws", "s3", "cp"])
            return {
                "sysroot-amd64-url": "https://bucket/sysroot-new.tar.gz",
                "sysroot-amd64-sha256": "new",
            }

        def fake_run(*args, **kwargs):
            command = args[0]
            if isinstance(command, list) and command[:3] == ["docker", "buildx", "build"]:
                distro = command[command.index("--tag") + 1].split(":")[-1].rsplit("-", 2)[0]
                built.append(distro)
                if distro == failing_distro:
                    raise subprocess.CalledProcessError(1, command)
            return mock.Mock(stdout="'[repo@sha256:abc123]'")

        with tempfile.TemporaryDirectory() as temp_dir:
            container_file = pathlib.Path(temp_dir) / "remote_execution_containers.bzl"
            container_file.write_text(f"REMOTE_EXECUTION_CONTAINERS = {containers!r}\n")
            with (
                mock.patch.object(sys, "argv", ["generator", "--skip-cleanup", *argv]),
                mock.patch.object(generator.os.path, "join", return_value=str(container_file)),
                mock.patch.object(generator, "resolve_dockerfile", side_effect=pathlib.Path),
                mock.patch.object(generator, "create_buildx_builder"),
                mock.patch.object(generator, "log_subprocess_run", side_effect=fake_run),
                mock.patch.object(generator, "publish_sysroots", side_effect=publish_sysroots),
                mock.patch("sys.stdout", new_callable=io.StringIO),
            ):
                exit_code = generator.main()

            written = {}
            exec(compile(container_file.read_text(), "containers", "exec"), {}, written)

        return exit_code, written["REMOTE_EXECUTION_CONTAINERS"], built

    def test_later_distros_still_update_after_a_failure(self) -> None:
        exit_code, containers, built = self._run_main(["--continue-on-error"], "debian12")

        self.assertEqual(["debian12", "ubuntu24"], built)
        self.assertEqual(1, exit_code, "the run must still fail the task")
        self.assertNotIn("container-url", containers["debian12"])
        self.assertEqual("docker://repo@sha256:abc123", containers["ubuntu24"]["container-url"])

    def test_failure_aborts_the_run_by_default(self) -> None:
        with self.assertRaises(subprocess.CalledProcessError):
            self._run_main([], "debian12")

    def test_failed_sysroot_upload_preserves_old_pins_and_updates_later_distros(self) -> None:
        old = {
            "dockerfile": "debian12/Dockerfile",
            "container-url": "docker://repo@sha256:old",
            "sysroot-amd64-url": "https://bucket/sysroot-old.tar.gz",
            "sysroot-amd64-sha256": "old",
        }
        exit_code, containers, built = self._run_main(
            ["--continue-on-error", "--sysroot-s3-bucket=bucket"],
            failing_upload_distro="debian12",
            containers={"debian12": old, "ubuntu24": {"dockerfile": "ubuntu24/Dockerfile"}},
        )
        self.assertEqual(1, exit_code)
        self.assertEqual(["debian12", "ubuntu24"], built)
        self.assertEqual(old, containers["debian12"])
        self.assertEqual("new", containers["ubuntu24"]["sysroot-amd64-sha256"])

    def test_container_only_update_drops_stale_sysroot_metadata(self) -> None:
        exit_code, containers, _ = self._run_main(
            [],
            containers={
                "debian12": {"dockerfile": "debian12/Dockerfile", "sysroot-amd64-sha256": "old"}
            },
        )
        self.assertEqual(0, exit_code)
        self.assertNotIn("sysroot-amd64-sha256", containers["debian12"])

    def test_dry_run_records_proposed_sysroot_metadata(self) -> None:
        exit_code, containers, _ = self._run_main(
            ["--sysroot-s3-bucket=bucket", "--sysroot-dry-run"]
        )
        self.assertEqual(0, exit_code)
        self.assertEqual("new", containers["debian12"]["sysroot-amd64-sha256"])


if __name__ == "__main__":
    unittest.main()
