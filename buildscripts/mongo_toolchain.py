import os
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional

sys.path.append("bazel")
from bazelisk import get_bazel_path, make_bazel_cmd

# WARNING: this file is imported from outside of any virtual env so the main import block MUST NOT
# import any third-party non-std libararies. Libraries needed when running as a script can be
# conditionally imported below.

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(os.path.realpath(__file__)))))


SUPPORTED_VERSIONS = "v5"


class MongoToolchainError(RuntimeError):
    pass


class MongoToolchainNotFoundError(MongoToolchainError):
    def __init__(self, extra_msg: str):
        super().__init__(extra_msg)


class ToolNotFoundError(MongoToolchainError):
    def __init__(self, tool: str, extra_msg: str | None):
        msg = f"Couldn't find {tool} in mongo toolchain"
        msg = msg + f": {extra_msg}" if extra_msg is not None else msg
        super().__init__(msg)


class MongoToolchain:
    def __init__(self, root_dir_path: Path):
        self._root_dir = root_dir_path

    def get_tool_path(self, tool: str) -> str:
        path = self._get_bin_dir_path() / tool
        if not path.exists():
            raise ToolNotFoundError(tool, f"{path} does not exist")
        if not path.is_file() or not os.access(path, os.X_OK):
            raise ToolNotFoundError(tool, f"{path} is not an executable")
        return str(path)

    def check_exists(self) -> None:
        for directory in (
            self._root_dir,
            self._get_bin_dir_path(),
            self._get_include_dir_path(),
            self._get_lib_dir_path(),
        ):
            if not directory.is_dir():
                raise MongoToolchainNotFoundError(f"{directory} is not a directory")

    def get_root_dir(self) -> str:
        return str(self._root_dir)

    def get_bin_dir(self) -> str:
        return str(self._get_bin_dir_path())

    def get_include_dir(self) -> str:
        return str(self._get_include_dir_path())

    def get_lib_dir(self) -> str:
        return str(self._get_include_dir_path())

    def _get_bin_dir_path(self) -> Path:
        return self._root_dir / "bin"

    def _get_include_dir_path(self) -> Path:
        return self._root_dir / "include"

    def _get_lib_dir_path(self) -> Path:
        return self._root_dir / "lib"


def _execute_bazel(argv):
    bazel_cmd = make_bazel_cmd(get_bazel_path(), argv)
    cmd = f"{bazel_cmd['exec']} {' '.join(bazel_cmd['args'])}"
    for i in range(5):
        try:
            return subprocess.check_output(
                cmd,
                env=bazel_cmd["env"],
                shell=True,
                text=True,
            ).strip()
        except subprocess.CalledProcessError as e:
            print(
                f"Failed to execute bazel command: `{e.cmd}` exited with code {e.returncode}, retrying in 60s..."
            )
            time.sleep(60)
            if i == 4:
                raise e


def _fetch_bazel_toolchain(version: str) -> None:
    try:
        _execute_bazel(
            ["build", "--bes_backend=", "--bes_results_url=", f"@mongo_toolchain_{version}//:all"]
        )
    except subprocess.CalledProcessError as e:
        raise MongoToolchainNotFoundError(
            f"Failed to fetch bazel toolchain: `{e.cmd}` exited with code {e.returncode}"
        )


def _get_bazel_execroot() -> Path:
    try:
        execroot_str = _execute_bazel(["info", "execution_root"])
    except subprocess.CalledProcessError as e:
        raise MongoToolchainNotFoundError(
            f"Couldn't find bazel execroot: `{e.cmd}` exited with code {e.returncode}"
        )
    return Path(execroot_str)


def _get_bazel_toolchain_path(version: str) -> Path:
    return _get_bazel_execroot() / "external" / f"mongo_toolchain_{version}" / version


def _get_toolchain_from_path(path: str | Path) -> MongoToolchain:
    toolchain = MongoToolchain(Path(path).resolve())
    toolchain.check_exists()
    return toolchain


def _get_bazel_toolchain(version: str) -> MongoToolchain:
    path = _get_bazel_toolchain_path(version)
    if not path.exists():
        _fetch_bazel_toolchain(version)
    if not path.is_dir():
        raise MongoToolchainNotFoundError(
            f"Couldn't find bazel toolchain: {path} is not a directory"
        )
    return _get_toolchain_from_path(path)


def _get_installed_toolchain_path(version: str) -> Path:
    return Path("/opt/mongodbtoolchain") / version


def _get_installed_toolchain(version: str):
    return _get_toolchain_from_path(_get_installed_toolchain_path(version))


def get_mongo_toolchain(
    *, version: str | None = None, from_bazel: bool | None = None
) -> MongoToolchain:
    # When running under bazel this environment variable will be set and will point to the
    # toolchain the target was configured to use. It can also be set manually to override
    # a script's selection of toolchain.
    toolchain_path = os.environ.get("MONGO_TOOLCHAIN_PATH", None)
    if toolchain_path is not None:
        return _get_toolchain_from_path(toolchain_path)

    # If no version given, look in the environment or default to v5.
    if version is None:
        version = os.environ.get("MONGO_TOOLCHAIN_VERSION", "v5")
    assert version is not None
    if version not in SUPPORTED_VERSIONS:
        raise MongoToolchainNotFoundError(f"Unknown toolchain version {version}")

    # If from_bazel is unspecified, let's query from bazel where querying toolchain
    # version is supported since v5.
    def _parse_from_bazel_envvar(value: str) -> bool:
        v = value.lower()
        if v in ("true", "1"):
            return True
        elif v in ("false", "0"):
            return False
        else:
            raise ValueError(f"Invalid value {value} for MONGO_TOOLCHAIN_FROM_BAZEL")

    if from_bazel is None:
        from_bazel_value = os.environ.get("MONGO_TOOLCHAIN_FROM_BAZEL", "true")
        from_bazel = _parse_from_bazel_envvar(from_bazel_value)

    if from_bazel:
        return _get_bazel_toolchain(version)
    return _get_installed_toolchain(version)


def try_get_mongo_toolchain(*args, **kwargs) -> MongoToolchain | None:
    try:
        return get_mongo_toolchain(*args, **kwargs)
    except MongoToolchainError:
        return None


if __name__ == "__main__":
    # See comment on main import block
    import typer
    from typing_extensions import Annotated

    _app = typer.Typer(add_completion=False)

    @_app.command()
    def main(
        tool: Annotated[Optional[str], typer.Argument()] = None,
        version: Annotated[Optional[str], typer.Option("--version")] = None,
        from_bazel: Annotated[Optional[bool], typer.Option("--bazel/--no-bazel")] = None,
    ):
        """
        Prints the path to tools in the mongo toolchain or the toolchain's root directory (which
        should contain bin/, include/, and so on).
        If MONGO_TOOLCHAIN_PATH is set in the environment, it overrides all options to this script.
        Otherwise, MONGO_TOOLCHAIN_VERSION is a lower-precedence alternative to --version and
        MONGO_TOOLCHAIN_FROM_BAZEL is a lower-precedence alternative to --bazel/--no-bazel.
        None of these are required, and if none are given, the default configuration will be used.
        """
        toolchain = get_mongo_toolchain(version=version, from_bazel=from_bazel)
        if tool is not None:
            print(toolchain.get_tool_path(tool))
        else:
            print(toolchain.get_root_dir())

    _app()
