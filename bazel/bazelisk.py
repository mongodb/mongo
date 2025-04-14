#!/usr/bin/env python3
"""
Copyright 2018 Google Inc. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
"""

import base64
import hashlib
import json
import netrc
import os
import os.path
import platform
import re
import shutil
import subprocess
import sys
import time
from contextlib import closing

try:
    from urllib.error import HTTPError
    from urllib.parse import urlparse
    from urllib.request import Request, urlopen
except ImportError:
    # Python 2.x compatibility hack.
    # http://python-future.org/compatible_idioms.html?highlight=urllib#urllib-module
    from urllib2 import HTTPError, Request, urlopen
    from urlparse import urlparse

    FileNotFoundError = IOError

ONE_HOUR = 1 * 60 * 60

LATEST_PATTERN = re.compile(r"latest(-(?P<offset>\d+))?$")

LAST_GREEN_COMMIT_PATH = "https://storage.googleapis.com/bazel-builds/last_green_commit/github.com/bazelbuild/bazel.git/publish-bazel-binaries"

BAZEL_GCS_PATH_PATTERN = (
    "https://storage.googleapis.com/bazel-builds/artifacts/{platform}/{commit}/bazel"
)

SUPPORTED_PLATFORMS = {"linux": "ubuntu1404", "windows": "windows", "darwin": "macos"}

TOOLS_BAZEL_PATH = "./tools/bazel"

BAZEL_REAL = "BAZEL_REAL"

BAZEL_UPSTREAM = "bazelbuild"

_dotfiles_dict = None


def get_dotfiles_dict():
    """Loads all supported dotfiles and returns a unified name=value dictionary
    for their config settings. The dictionary is only loaded on the first call
    to this function; subsequent calls used a cached result, so won't change.
    """
    global _dotfiles_dict
    if _dotfiles_dict is not None:
        return _dotfiles_dict
    _dotfiles_dict = dict()
    env_files = []
    # Here we're only checking the workspace root. Ideally, we would also check
    # the user's home directory to match the Go version. When making that edit,
    # be sure to obey the correctly prioritization of workspace / home rcfiles.
    root = find_workspace_root()
    if root:
        env_files.append(os.path.join(root, ".bazeliskrc"))
    for env_file in env_files:
        try:
            with open(env_file, "r") as f:
                rcdata = f.read()
        except Exception:
            continue
        for line in rcdata.splitlines():
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            some_name, some_value = line.split("=", 1)
            _dotfiles_dict[some_name] = some_value
    return _dotfiles_dict


def get_env_or_config(name, default=None):
    """Reads a configuration value from the environment, but falls back to
    reading it from .bazeliskrc in the workspace root."""
    if name in os.environ:
        return os.environ[name]
    dotfiles = get_dotfiles_dict()
    if name in dotfiles:
        return dotfiles[name]
    return default


def decide_which_bazel_version_to_use():
    # Check in this order:
    # - env var "USE_BAZEL_VERSION" is set to a specific version.
    # - env var "USE_NIGHTLY_BAZEL" or "USE_BAZEL_NIGHTLY" is set -> latest
    #   nightly. (TODO)
    # - env var "USE_CANARY_BAZEL" or "USE_BAZEL_CANARY" is set -> latest
    #   rc. (TODO)
    # - the file workspace_root/tools/bazel exists -> that version. (TODO)
    # - workspace_root/.bazelversion exists -> read contents, that version.
    # - workspace_root/WORKSPACE contains a version -> that version. (TODO)
    # - fallback: latest release
    use_bazel_version = get_env_or_config("USE_BAZEL_VERSION")
    if use_bazel_version is not None:
        return use_bazel_version

    workspace_root = find_workspace_root()
    if workspace_root:
        bazelversion_path = os.path.join(workspace_root, ".bazelversion")
        if os.path.exists(bazelversion_path):
            with open(bazelversion_path, "r") as f:
                for ln in f.readlines():
                    ln = ln.strip()
                    if ln:
                        return ln

    return "latest"


def find_workspace_root(root=None):
    if root is None:
        root = os.getcwd()
    for boundary in ["MODULE.bazel", "REPO.bazel", "WORKSPACE.bazel", "WORKSPACE"]:
        path = os.path.join(root, boundary)
        if os.path.exists(path) and not os.path.isdir(path):
            return root
    new_root = os.path.dirname(root)
    return find_workspace_root(new_root) if new_root != root else None


def resolve_version_label_to_number_or_commit(bazelisk_directory, version):
    """Resolves the given label to a released version of Bazel or a commit.

    Args:
        bazelisk_directory: string; path to a directory that can store
            temporary data for Bazelisk.
        version: string; the version label that should be resolved.
    Returns:
        A (string, bool) tuple that consists of two parts:
        1. the resolved number of a Bazel release (candidate), or the commit
            of an unreleased Bazel binary,
        2. An indicator for whether the returned version refers to a commit.
    """
    if version == "last_green":
        return get_last_green_commit(), True

    if "latest" in version:
        match = LATEST_PATTERN.match(version)
        if not match:
            raise Exception(
                'Invalid version "{}". In addition to using a version '
                'number such as "0.20.0", you can use values such as '
                '"latest" and "latest-N", with N being a non-negative '
                "integer.".format(version)
            )

        history = get_version_history(bazelisk_directory)
        offset = int(match.group("offset") or "0")
        return resolve_latest_version(history, offset), False

    return version, False


def get_last_green_commit():
    commit = read_remote_text_file(LAST_GREEN_COMMIT_PATH).strip()
    if not re.match(r"^[0-9a-f]{40}$", commit):
        raise Exception("Invalid commit hash: {}".format(commit))
    return commit


def get_releases_json(bazelisk_directory):
    """Returns the most recent versions of Bazel, in descending order."""
    releases = os.path.join(bazelisk_directory, "releases.json")

    # Use a cached version if it's fresh enough.
    if os.path.exists(releases):
        if abs(time.time() - os.path.getmtime(releases)) < ONE_HOUR:
            with open(releases, "rb") as f:
                try:
                    return json.loads(f.read().decode("utf-8"))
                except ValueError:
                    print("WARN: Could not parse cached releases.json.")
                    pass

    with open(releases, "wb") as f:
        body = read_remote_text_file("https://api.github.com/repos/bazelbuild/bazel/releases")
        f.write(body.encode("utf-8"))
        return json.loads(body)


def read_remote_text_file(url):
    with closing(urlopen(url)) as res:
        body = res.read()
        try:
            return body.decode(res.info().get_content_charset("iso-8859-1"))
        except AttributeError:
            # Python 2.x compatibility hack
            return body.decode(res.info().getparam("charset") or "iso-8859-1")


def get_version_history(bazelisk_directory):
    return sorted(
        (
            release["tag_name"]
            for release in get_releases_json(bazelisk_directory)
            if not release["prerelease"]
        ),
        # This only handles versions with numeric components, but that is fine
        # since prerelease versions have been excluded.
        key=lambda version: tuple(int(component) for component in version.split(".")),
        reverse=True,
    )


def resolve_latest_version(version_history, offset):
    if offset >= len(version_history):
        version = "latest-{}".format(offset) if offset else "latest"
        raise Exception(
            'Cannot resolve version "{}": There are only {} Bazel releases.'.format(
                version, len(version_history)
            )
        )

    # This only works since we store the history in descending order.
    return version_history[offset]


def get_operating_system():
    operating_system = platform.system().lower()
    if operating_system not in ("linux", "darwin", "windows"):
        raise Exception(
            'Unsupported operating system "{}". '
            "Bazel currently only supports Linux, macOS and Windows.".format(operating_system)
        )
    return operating_system


def determine_executable_filename_suffix():
    operating_system = get_operating_system()
    return ".exe" if operating_system == "windows" else ""


def determine_bazel_filename(version):
    operating_system = get_operating_system()
    supported_machines = get_supported_machine_archs(version, operating_system)
    machine = normalized_machine_arch_name()
    if machine not in supported_machines:
        raise Exception(
            'Unsupported machine architecture "{}". Bazel {} only supports {} on {}.'.format(
                machine, version, ", ".join(supported_machines), operating_system.capitalize()
            )
        )

    filename_suffix = determine_executable_filename_suffix()
    bazel_flavor = "bazel"
    if get_env_or_config("BAZELISK_NOJDK", "0") != "0":
        bazel_flavor = "bazel_nojdk"
    return "{}-{}-{}-{}{}".format(bazel_flavor, version, operating_system, machine, filename_suffix)


def get_supported_machine_archs(version, operating_system):
    supported_machines = ["x86_64"]
    if operating_system == "linux":
        supported_machines += ["s390x", "ppc64le"]
    versions = version.split(".")[:2]
    if len(versions) == 2:
        # released version
        major, minor = int(versions[0]), int(versions[1])
        if (
            operating_system == "darwin"
            and (major > 4 or major == 4 and minor >= 1)
            or operating_system == "linux"
            and (major > 3 or major == 3 and minor >= 4)
        ):
            # Linux arm64 was supported since 3.4.0.
            # Darwin arm64 was supported since 4.1.0.
            supported_machines.append("arm64")
    elif operating_system in ("darwin", "linux"):
        # This is needed to run bazelisk_test.sh on Linux and Darwin arm64 machines, which are
        # becoming more and more popular.
        # It works because all recent commits of Bazel support arm64 on Darwin and Linux.
        # However, this would add arm64 by mistake if the commit is too old, which should be
        # a rare scenario.
        supported_machines.append("arm64")
    return supported_machines


def normalized_machine_arch_name():
    machine = platform.machine().lower()
    if machine == "amd64":
        machine = "x86_64"
    elif machine == "aarch64":
        machine = "arm64"
    return machine


def determine_url(version, is_commit, bazel_filename):
    if is_commit:
        sys.stderr.write("Using unreleased version at commit {}\n".format(version))
        # No need to validate the platform thanks to determine_bazel_filename().
        return BAZEL_GCS_PATH_PATTERN.format(
            platform=SUPPORTED_PLATFORMS[platform.system().lower()], commit=version
        )

    # Split version into base version and optional additional identifier.
    # Example: '0.19.1' -> ('0.19.1', None), '0.20.0rc1' -> ('0.20.0', 'rc1')
    (version, rc, mongo_version) = re.match(
        r"(\d*\.\d*(?:\.\d*)?)(rc\d+)?(-mongo_\w+)?", version
    ).groups()

    bazelisk_base_url = get_env_or_config("BAZELISK_BASE_URL")
    if bazelisk_base_url is not None:
        if mongo_version:
            version += mongo_version
        return "{}/{}/{}".format(bazelisk_base_url, version, bazel_filename)
    else:
        return "https://releases.bazel.build/{}/{}/{}".format(
            version, rc if rc else "release", bazel_filename
        )


def trim_suffix(string, suffix):
    if string.endswith(suffix):
        return string[: len(string) - len(suffix)]
    else:
        return string


def download_bazel_into_directory(version, is_commit, directory):
    bazel_filename = determine_bazel_filename(version)
    bazel_url = determine_url(version, is_commit, bazel_filename)

    filename_suffix = determine_executable_filename_suffix()
    bazel_directory_name = trim_suffix(bazel_filename, filename_suffix)
    destination_dir = os.path.join(directory, bazel_directory_name, "bin")
    maybe_makedirs(destination_dir)

    destination_path = os.path.join(destination_dir, "bazel" + filename_suffix)
    if not os.path.exists(destination_path):
        download(bazel_url, destination_path)
        os.chmod(destination_path, 0o755)

    sha256_path = destination_path + ".sha256"
    expected_hash = ""
    if not os.path.exists(sha256_path):
        try:
            download(bazel_url + ".sha256", sha256_path)
        except HTTPError as e:
            if e.code == 404:
                sys.stderr.write(
                    "The Bazel mirror does not have a checksum file; skipping checksum verification."
                )
                return destination_path
            raise e
    with open(sha256_path, "r") as sha_file:
        expected_hash = sha_file.read().split()[0]
    sha256_hash = hashlib.sha256()
    with open(destination_path, "rb") as bazel_file:
        for byte_block in iter(lambda: bazel_file.read(4096), b""):
            sha256_hash.update(byte_block)
    actual_hash = sha256_hash.hexdigest()
    if actual_hash != expected_hash:
        os.remove(destination_path)
        os.remove(sha256_path)
        print(
            "The downloaded Bazel binary is corrupted. Expected SHA-256 {}, got {}. Please try again.".format(
                expected_hash, actual_hash
            )
        )
        # Exiting with a special exit code not used by Bazel, so the calling process may retry based on that.
        # https://docs.bazel.build/versions/0.21.0/guide.html#what-exit-code-will-i-get
        sys.exit(22)
    return destination_path


def download(url, destination_path):
    sys.stderr.write("Downloading {}...\n".format(url))
    request = Request(url)
    if get_env_or_config("BAZELISK_BASE_URL") is not None:
        parts = urlparse(url)
        creds = None
        try:
            creds = netrc.netrc().hosts.get(parts.netloc)
        except Exception:
            pass
        if creds is not None:
            auth = base64.b64encode(("%s:%s" % (creds[0], creds[2])).encode("ascii"))
            request.add_header("Authorization", "Basic %s" % auth.decode("utf-8"))
    with closing(urlopen(request)) as response, open(destination_path, "wb") as file:
        shutil.copyfileobj(response, file)


def get_bazelisk_directory():
    bazelisk_home = get_env_or_config("BAZELISK_HOME")
    if bazelisk_home is not None:
        return bazelisk_home

    operating_system = get_operating_system()

    base_dir = None

    if operating_system == "windows":
        base_dir = os.environ.get("LocalAppData")
        if base_dir is None:
            raise Exception("%LocalAppData% is not defined")
    elif operating_system == "darwin":
        base_dir = os.environ.get("HOME")
        if base_dir is None:
            raise Exception("$HOME is not defined")
        base_dir = os.path.join(base_dir, "Library/Caches")
    elif operating_system == "linux":
        base_dir = os.environ.get("XDG_CACHE_HOME")
        if base_dir is None:
            base_dir = os.environ.get("HOME")
            if base_dir is None:
                raise Exception("neither $XDG_CACHE_HOME nor $HOME are defined")
            base_dir = os.path.join(base_dir, ".cache")
    else:
        raise Exception("Unsupported operating system '{}'".format(operating_system))

    return os.path.join(base_dir, "bazelisk")


def maybe_makedirs(path):
    """
    Creates a directory and its parents if necessary.
    """
    try:
        os.makedirs(path)
    except OSError as e:
        if not os.path.isdir(path):
            raise e


def delegate_tools_bazel(bazel_path):
    """Match Bazel's own delegation behavior in the builds distributed by most
    package managers: use tools/bazel if it's present, executable, and not this
    script.
    """
    root = find_workspace_root()
    if root:
        wrapper = os.path.join(root, TOOLS_BAZEL_PATH)
        if os.path.exists(wrapper) and os.access(wrapper, os.X_OK):
            try:
                if not os.path.samefile(wrapper, __file__):
                    return wrapper
            except AttributeError:
                # Python 2 on Windows does not support os.path.samefile
                if os.path.abspath(wrapper) != os.path.abspath(__file__):
                    return wrapper
    return None


def prepend_directory_to_path(env, directory):
    """
    Prepend binary directory to PATH
    """
    if "PATH" in env:
        env["PATH"] = directory + os.pathsep + env["PATH"]
    else:
        env["PATH"] = directory


def make_bazel_cmd(bazel_path, argv):
    env = os.environ.copy()

    wrapper = delegate_tools_bazel(bazel_path)
    if wrapper:
        env[BAZEL_REAL] = bazel_path
        env["BAZELISK_SKIP_WRAPPER"] = "1"
        bazel_path = wrapper

    directory = os.path.dirname(bazel_path)
    prepend_directory_to_path(env, directory)
    return {
        "exec": bazel_path,
        "args": argv,
        "env": env,
    }


def execute_bazel(bazel_path, argv):
    cmd = make_bazel_cmd(bazel_path, argv)

    # We cannot use close_fds on Windows, so disable it there.
    p = subprocess.Popen([cmd["exec"]] + cmd["args"], close_fds=os.name != "nt", env=cmd["env"])
    while True:
        try:
            return p.wait()
        except KeyboardInterrupt:
            # Bazel will also get the signal and terminate.
            # We should continue waiting until it does so.
            pass


def get_bazel_path():
    bazelisk_directory = get_bazelisk_directory()
    maybe_makedirs(bazelisk_directory)

    bazel_version = decide_which_bazel_version_to_use()
    bazel_version, is_commit = resolve_version_label_to_number_or_commit(
        bazelisk_directory, bazel_version
    )

    # TODO: Support other forks just like Go version
    bazel_directory = os.path.join(bazelisk_directory, "downloads", BAZEL_UPSTREAM)
    return download_bazel_into_directory(bazel_version, is_commit, bazel_directory)


def main(argv=None):
    if argv is None:
        argv = sys.argv

    bazel_path = get_bazel_path()

    argv = argv[1:]

    if argv and argv[0] == "--print_env":
        cmd = make_bazel_cmd(bazel_path, argv)
        env = cmd["env"]
        for key in env:
            print("{}={}".format(key, env[key]))
        return 0

    return execute_bazel(bazel_path, argv)


if __name__ == "__main__":
    sys.exit(main())
