"""EngFlow authentication helpers for container builds."""

from __future__ import annotations

import os
import pathlib
import platform
import shlex
import subprocess
import urllib.request
from collections.abc import Mapping

from .distro import normalize_arch
from .download import _default_container_bazel_arch
from .env import _env_is_false
from .fsutil import _hermetic_container_home_dir, _hermetic_container_state_dir, _sha256_file
from .volumes import _container_path

HERMETIC_CONTAINER_ENGFLOW_AUTH_HELPER_ENV = "MONGO_HERMETIC_CONTAINER_ENGFLOW_AUTH_HELPER"


ENGFLOW_AUTH_CLUSTER = "sodalite.cluster.engflow.com"


ENGFLOW_AUTH_URL_PREFIX = "https://github.com/EngFlow/auth/releases/download/v0.0.13/"


ENGFLOW_AUTH_LINUX_RELEASES = {
    "aarch64": (
        "engflow_auth_linux_arm64",
        "ad5ffee1e6db926f5066aa40ee35517b1993851d0063ac121dbf5b407c81e2bf",
    ),
    "x86_64": (
        "engflow_auth_linux_x64",
        "b731bae21628b2be321c24b342854c6ed1ed0326010e62a2ecf0b5650a56cf1a",
    ),
}


def _host_engflow_auth_helper(repo_root: pathlib.Path) -> pathlib.Path | None:
    bazelrc = repo_root / ".bazelrc.engflow_creds"
    try:
        lines = bazelrc.read_text(encoding="utf-8").splitlines()
    except OSError:
        return None

    prefix = f"--credential_helper={ENGFLOW_AUTH_CLUSTER}="
    for line in lines:
        try:
            tokens = shlex.split(line)
        except ValueError:
            continue
        for token in tokens:
            if token.startswith(prefix):
                return pathlib.Path(token[len(prefix) :])
    return None


def _host_engflow_env(env: Mapping[str, str]) -> dict[str, str]:
    result = dict(env)
    if platform.system() != "Windows":
        return result

    profile = result.get("USERPROFILE")
    if not profile and result.get("HOMEDRIVE") and result.get("HOMEPATH"):
        profile = result["HOMEDRIVE"] + result["HOMEPATH"]
    profile = profile or r"C:\Users\Administrator"
    result.setdefault("USERPROFILE", profile)
    result.setdefault("HOME", profile)
    result.setdefault("APPDATA", os.path.join(profile, "AppData", "Roaming"))
    result.setdefault("LOCALAPPDATA", os.path.join(profile, "AppData", "Local"))
    return result


def _sync_engflow_file_token(
    repo_root: pathlib.Path,
    env: Mapping[str, str],
    token_home: pathlib.Path,
) -> None:
    if env.get("MONGO_HERMETIC_CONTAINER_DRY_RUN") == "1":
        return

    host_helper = _host_engflow_auth_helper(repo_root)
    if host_helper is None or not os.access(host_helper, os.X_OK):
        return

    export = subprocess.run(
        [str(host_helper), "export", ENGFLOW_AUTH_CLUSTER],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=_host_engflow_env(os.environ),
    )
    if export.returncode or not export.stdout.strip():
        return

    token_home.mkdir(parents=True, exist_ok=True)
    import_env = _host_engflow_env(os.environ)
    import_env["HOME"] = str(token_home)
    import_env["USERPROFILE"] = str(token_home)
    import_env["APPDATA"] = str(token_home / "AppData" / "Roaming")
    import_env["LOCALAPPDATA"] = str(token_home / "AppData" / "Local")
    pathlib.Path(import_env["APPDATA"]).mkdir(parents=True, exist_ok=True)
    pathlib.Path(import_env["LOCALAPPDATA"]).mkdir(parents=True, exist_ok=True)
    import_result = subprocess.run(
        [str(host_helper), "import", "--store=file"],
        check=False,
        input=export.stdout,
        env=import_env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if import_result.returncode:
        return

    macos_token = (
        token_home
        / "Library"
        / "Application Support"
        / "engflow_auth"
        / "tokens"
        / ENGFLOW_AUTH_CLUSTER
    )
    linux_token = token_home / ".config" / "engflow_auth" / "tokens" / ENGFLOW_AUTH_CLUSTER
    windows_token = (
        token_home / "AppData" / "Roaming" / "engflow_auth" / "tokens" / ENGFLOW_AUTH_CLUSTER
    )
    source_token = next((path for path in [macos_token, windows_token] if path.exists()), None)
    if source_token is None:
        return

    linux_token.parent.mkdir(parents=True, exist_ok=True)
    linux_token.write_bytes(source_token.read_bytes())
    linux_token.chmod(0o600)


def _hermetic_container_engflow_auth_helper(
    repo_root: pathlib.Path,
    env: Mapping[str, str],
    system: str,
) -> str:
    override = env.get(HERMETIC_CONTAINER_ENGFLOW_AUTH_HELPER_ENV)
    if _env_is_false(override):
        return ""
    if override:
        return _container_path(override, system)

    if system not in {"Darwin", "Windows"}:
        return ""
    if not (repo_root / ".bazelrc.engflow_creds").exists():
        return ""

    _sync_engflow_file_token(repo_root, env, _hermetic_container_home_dir(repo_root))

    arch = normalize_arch(
        env.get("MONGO_HERMETIC_CONTAINER_CONTAINER_ARCH", _default_container_bazel_arch(system))
    )
    if arch not in ENGFLOW_AUTH_LINUX_RELEASES:
        return ""

    release_name, expected_sha256 = ENGFLOW_AUTH_LINUX_RELEASES[arch]
    helper = (
        _hermetic_container_state_dir(repo_root) / "engflow_auth" / release_name / "engflow_auth"
    )
    if env.get("MONGO_HERMETIC_CONTAINER_DRY_RUN") == "1":
        return _container_path(helper, system)
    if helper.exists() and _sha256_file(helper) == expected_sha256:
        helper.chmod(0o500)
        return _container_path(helper, system)

    helper.parent.mkdir(parents=True, exist_ok=True)
    tmp_helper = helper.with_suffix(".tmp")
    urllib.request.urlretrieve(ENGFLOW_AUTH_URL_PREFIX + release_name, tmp_helper)
    if _sha256_file(tmp_helper) != expected_sha256:
        tmp_helper.unlink(missing_ok=True)
        raise RuntimeError(f"Downloaded {release_name} does not match expected checksum")
    tmp_helper.chmod(0o500)
    tmp_helper.replace(helper)
    return _container_path(helper, system)
