"""Convenience and shared-install symlink management."""

from __future__ import annotations

import json
import os
import pathlib
import shutil
from collections.abc import Mapping, Sequence

from .bazelrc import _bazel_output_base
from .config import HermeticContainerConfig, _hermetic_container_output_base
from .constants import (
    HERMETIC_CONTAINER_CONVENIENCE_SYMLINKS_ENV,
    HERMETIC_CONTAINER_ROOT_CONVENIENCE_SYMLINKS,
    HERMETIC_CONTAINER_SYMLINK_PREFIX,
    LINUX_HOST_CONTAINER_NO_RUNTIME_COMMANDS,
    REPO_ROOT,
)
from .env import _bazel_command_index, _info
from .fsutil import _is_relative_to, _macos_shared_install_dir, _symlink_target


def _bazel_args_with_hermetic_container_symlink_prefix(args: Sequence[str]) -> list[str]:
    command_index = _bazel_command_index(args)
    if command_index is None:
        return list(args)

    # Graph and repository-fetch commands do not execute actions. In particular,
    # query-family commands reject build-only options such as --symlink_prefix, so
    # leave their command line untouched on the Linux host integration path.
    if args[command_index] in LINUX_HOST_CONTAINER_NO_RUNTIME_COMMANDS:
        return list(args)

    for arg in args[command_index + 1 :]:
        if arg == "--":
            break
        if arg == "--symlink_prefix" or arg.startswith("--symlink_prefix="):
            return list(args)

    return [
        *args[: command_index + 1],
        f"--symlink_prefix={HERMETIC_CONTAINER_SYMLINK_PREFIX}",
        *args[command_index + 1 :],
    ]


def _workspace_convenience_symlink_name(repo_root: pathlib.Path) -> str:
    return f"{HERMETIC_CONTAINER_SYMLINK_PREFIX}{repo_root.name}"


def _managed_convenience_symlink_targets(
    repo_root: pathlib.Path = REPO_ROOT,
) -> dict[str, str]:
    convenience_dir = repo_root / pathlib.Path(HERMETIC_CONTAINER_SYMLINK_PREFIX).parent
    targets: dict[str, str] = {}
    names = (
        *HERMETIC_CONTAINER_ROOT_CONVENIENCE_SYMLINKS,
        _workspace_convenience_symlink_name(repo_root),
    )
    for name in names:
        target = _symlink_target(convenience_dir / name)
        if target is not None:
            targets[name] = str(target)
    return targets


def _hermetic_container_convenience_symlink_targets(
    config: HermeticContainerConfig,
    docker_instance: object,
    repo_root: pathlib.Path = REPO_ROOT,
) -> dict[str, str]:
    output_base = _hermetic_container_output_base(config, docker_instance).resolve()
    execroot = output_base / "execroot" / "_main"
    targets = {
        name: target
        for name, target in _managed_convenience_symlink_targets(repo_root).items()
        if _is_relative_to(pathlib.Path(target).resolve(), output_base)
    }
    targets.setdefault("bazel-out", str(execroot / "bazel-out"))

    bazel_bin = pathlib.Path(targets.get("bazel-bin", ""))
    if "bazel-testlogs" not in targets and bazel_bin.name == "bin":
        targets["bazel-testlogs"] = str(bazel_bin.parent / "testlogs")

    targets.setdefault(_workspace_convenience_symlink_name(repo_root), str(execroot))
    return targets


def _write_hermetic_container_convenience_symlink_marker(
    links: Mapping[str, str],
    env: Mapping[str, str],
    repo_root: pathlib.Path = REPO_ROOT,
) -> None:
    marker = env.get(HERMETIC_CONTAINER_CONVENIENCE_SYMLINKS_ENV)
    if not marker:
        return

    marker_path = pathlib.Path(marker)
    data = {
        "repo_root": str(repo_root),
        "links": dict(links),
    }
    marker_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_marker = marker_path.with_suffix(marker_path.suffix + ".tmp")
    tmp_marker.write_text(json.dumps(data, sort_keys=True), encoding="utf-8")
    tmp_marker.replace(marker_path)


def _replace_symlink(link: pathlib.Path, target: pathlib.Path) -> None:
    if link.is_symlink() or link.is_file():
        link.unlink()
    elif link.exists():
        _info(f"not replacing non-symlink convenience path: {link}")
        return
    link.symlink_to(target, target_is_directory=True)


def _publish_convenience_symlinks(
    links: Mapping[str, str],
    env: Mapping[str, str],
    repo_root: pathlib.Path = REPO_ROOT,
) -> None:
    for name, target in links.items():
        _replace_symlink(repo_root / name, pathlib.Path(target))
    _write_hermetic_container_convenience_symlink_marker(links, env, repo_root)


def _publish_hermetic_container_convenience_symlinks(
    config: HermeticContainerConfig,
    docker_instance: object,
    env: Mapping[str, str],
    repo_root: pathlib.Path = REPO_ROOT,
) -> None:
    _publish_convenience_symlinks(
        _hermetic_container_convenience_symlink_targets(config, docker_instance, repo_root),
        env,
        repo_root,
    )


def _publish_linux_host_convenience_symlinks(
    env: Mapping[str, str], repo_root: pathlib.Path = REPO_ROOT
) -> None:
    _publish_convenience_symlinks(
        _managed_convenience_symlink_targets(repo_root),
        env,
        repo_root,
    )


def _publish_linux_shared_install_symlink(
    config: Mapping[str, str], repo_root: pathlib.Path = REPO_ROOT
) -> None:
    target_value = config.get("shared_install_dir")
    # Test-only materialization can request a specific private tree when another install target
    # already exists in the same output base. This keeps ``bazel-bin/install`` aligned with the
    # target whose test list the caller is about to consume.
    preferred_install_target = config.get("preferred_install_target") or os.environ.get(
        "MONGO_BAZEL_INSTALL_TARGET", ""
    )
    preferred_install_name = pathlib.Path(preferred_install_target).name
    if not preferred_install_name.startswith("install-") and preferred_install_name != "install":
        preferred_install_name = ""
    bazel_bin = repo_root / "bazel-bin"
    bazel_bin_target = _symlink_target(bazel_bin)
    original_bazel_bin_target = bazel_bin_target
    if bazel_bin_target is not None:
        # Bazel normally writes an absolute convenience link, but some output-root
        # configurations use a relative target. Normalize it before inspecting the
        # expected ``bazel-out/<configuration>/bin`` layout.
        bazel_bin_target = bazel_bin_target.resolve()
    if not target_value:
        return

    shared_install_root = pathlib.Path(target_value)
    output_bin_candidates: list[pathlib.Path] = []

    def add_output_bin_candidate(candidate: pathlib.Path) -> None:
        candidate = candidate.resolve()
        if candidate not in output_bin_candidates and candidate.name == "bin":
            output_bin_candidates.append(candidate)

    if bazel_bin_target is not None:
        add_output_bin_candidate(bazel_bin_target)

    # A Bazel invocation running through a persistent container can leave bazel-bin pointing at
    # a path that is not visible from the host process. The output base itself is still shared,
    # so recover the configuration/bin directory from its stable execroot layout instead of
    # abandoning the convenience link.
    output_base_value = config.get("output_base")
    if output_base_value:
        output_base = pathlib.Path(output_base_value)
        for execroot in sorted((output_base / "execroot").glob("*")):
            for candidate in sorted((execroot / "bazel-out").glob("*/bin")):
                if candidate.is_dir():
                    add_output_bin_candidate(candidate)

    def private_install_targets(output_bin: pathlib.Path) -> list[pathlib.Path]:
        """Return action-private MongoInstallRule trees visible to the host.

        Most actions publish through the shared install root, but a persistent container can
        finish with only its declared private output visible to the host.  Keep the fallback
        generic so test-only install targets (notably dbtest and mongo_integration_test) are not
        mistaken for an empty install tree.
        """
        try:
            return sorted(
                child
                for child in output_bin.iterdir()
                if child.is_dir() and (child.name == "install" or child.name.startswith("install-"))
            )
        except OSError:
            return []

    def prioritize_private_install_targets(
        targets: list[pathlib.Path],
    ) -> list[pathlib.Path]:
        if not preferred_install_name:
            return targets
        return sorted(targets, key=lambda target: target.name != preferred_install_name)

    def tree_has_files(target: pathlib.Path) -> bool:
        try:
            if not target.is_dir():
                return False
            return any(
                child.is_file() or child.is_symlink() or tree_has_files(child)
                for child in target.iterdir()
            )
        except OSError:
            return False

    def private_install_has_outputs(target: pathlib.Path) -> bool:
        # A stale legacy ``bazel-bin/install`` directory may contain unrelated files at its root.
        # Only treat an action-private install tree as usable when it has installed binaries or a
        # generated resmoke test list.
        if not target.is_dir():
            return False
        try:
            return tree_has_files(target / "bin") or any(
                child.is_file() and child.name.endswith("_test_list.txt")
                for child in target.iterdir()
            )
        except OSError:
            return False

    def select_private_install_target(
        targets: list[pathlib.Path],
    ) -> pathlib.Path | None:
        if preferred_install_name:
            preferred = next(
                (target for target in targets if target.name == preferred_install_name), None
            )
            if preferred is not None and private_install_has_outputs(preferred):
                return preferred
        return next(
            (target for target in targets if (target / "bin" / "mongod").exists()),
            next((target for target in targets if private_install_has_outputs(target)), None),
        )

    configuration_name = None
    if (
        bazel_bin_target is not None
        and bazel_bin_target.is_dir()
        and bazel_bin_target.name == "bin"
    ):
        # Bazel-bin normally points at .../bazel-out/<configuration>/bin. Do not require the
        # parent to literally be named ``bazel-out``: output roots created by older Bazel
        # versions and test harnesses can include an additional execroot component.
        configuration_name = bazel_bin_target.parent.name
    else:
        # A read-only output root may prevent Bazel from publishing ``bazel-bin`` at all, and
        # archive tasks can leave a plain directory there before the wrapper runs. Recover the
        # configuration from the tree that MongoInstallRule populated instead of silently
        # leaving the post-build convenience path missing.
        try:
            candidates = [
                child
                for child in shared_install_root.iterdir()
                if child.is_dir() and not child.name.startswith(".")
            ]
        except OSError:
            return
        populated = [child for child in candidates if (child / "bin" / "mongod").exists()]
        if len(populated) == 1:
            configuration_name = populated[0].name
        elif len(candidates) == 1:
            configuration_name = candidates[0].name
    if not configuration_name:
        # Prefer an output configuration that contains the install action's private mongod. This
        # also handles a dangling/missing bazel-bin link when the shared tree was not populated by
        # an older persistent action container.
        for candidate in output_bin_candidates:
            private_targets = prioritize_private_install_targets(private_install_targets(candidate))
            if select_private_install_target(private_targets) is not None:
                configuration_name = candidate.parent.name
                bazel_bin_target = candidate
                break
    if not configuration_name and len(output_bin_candidates) == 1:
        configuration_name = output_bin_candidates[0].parent.name
        bazel_bin_target = output_bin_candidates[0]
    if not configuration_name:
        return

    # If the original convenience link was dangling, prefer the recovered output tree for the
    # selected configuration so we can restore the Bazel convenience link instead of replacing
    # it with a real directory.
    if bazel_bin_target is None or not bazel_bin_target.is_dir():
        bazel_bin_target = next(
            (
                candidate
                for candidate in output_bin_candidates
                if candidate.parent.name == configuration_name
            ),
            bazel_bin_target,
        )

    # The shared install root is writable because the output base is mounted read-only in the
    # action container; keep the same configuration boundary that Bazel normally provides under
    # bazel-out.
    shared_install_dir = shared_install_root / configuration_name
    shared_install_dir.mkdir(parents=True, exist_ok=True)

    install_target = shared_install_dir
    if bazel_bin_target is not None and not tree_has_files(install_target):
        # A persistent container can complete the declared MongoInstallRule outputs without
        # exposing MONGO_BAZEL_SHARED_INSTALL_DIR to the action. In that case the regular
        # install trees are still usable; prefer one over publishing a link to an empty shared
        # directory so post-build version/archive steps and test-only consumers keep working.
        private_targets = prioritize_private_install_targets(
            private_install_targets(bazel_bin_target)
        )
        install_target = select_private_install_target(private_targets) or install_target

    # Preserve an existing Bazel convenience symlink, but restore a recovered output-tree link
    # when the original link was dangling. Only fall back to a host-side directory when no output
    # tree could be recovered.
    if bazel_bin.is_symlink() and not bazel_bin.is_dir():
        # A dangling Bazel convenience link is unusable to callers and can occur when the link's
        # target was created only inside a persistent action container. Replace only that broken
        # link; preserve a live, non-directory target rather than overwriting unrelated paths.
        if original_bazel_bin_target is None or not original_bazel_bin_target.resolve().is_dir():
            bazel_bin.unlink()
        else:
            return
    if not bazel_bin.exists():
        if bazel_bin_target is not None and bazel_bin_target.is_dir():
            bazel_bin.symlink_to(bazel_bin_target, target_is_directory=True)
        else:
            bazel_bin.mkdir(parents=True, exist_ok=True)
    elif not bazel_bin.is_dir():
        return

    link = bazel_bin / "install"
    if link.is_symlink():
        # A live convenience link may already point at the selected install tree. Leave it in
        # place because the output root can be read-only from the host, making an unnecessary
        # unlink fail even though the published path is correct.
        try:
            if link.resolve() == install_target.resolve():
                return
        except OSError:
            pass
        link.unlink()
    elif link.is_file():
        link.unlink()
    elif link.is_dir():
        # This is generated output from the legacy unsandboxed install action.
        shutil.rmtree(link)
    link.symlink_to(install_target, target_is_directory=True)


def _publish_macos_shared_install_symlink(
    args: Sequence[str],
    env: Mapping[str, str],
    repo_root: pathlib.Path = REPO_ROOT,
) -> None:
    """Publish bazel-bin/install after Darwin actions use an external shared tree."""
    _publish_linux_shared_install_symlink(
        {"shared_install_dir": str(_macos_shared_install_dir(_bazel_output_base(args, env)))},
        repo_root=repo_root,
    )


def restore_hermetic_container_convenience_symlinks_from_env(
    env: Mapping[str, str] = os.environ,
) -> None:
    marker = env.get(HERMETIC_CONTAINER_CONVENIENCE_SYMLINKS_ENV)
    if not marker:
        return

    marker_path = pathlib.Path(marker)
    try:
        data = json.loads(marker_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return

    repo_root_value = data.get("repo_root", "")
    if not isinstance(repo_root_value, str) or not repo_root_value:
        return
    repo_root = pathlib.Path(repo_root_value)

    links = data.get("links", {})
    if not isinstance(links, dict):
        return

    for name, target in links.items():
        if not isinstance(name, str) or not isinstance(target, str):
            continue
        _replace_symlink(repo_root / name, pathlib.Path(target))
