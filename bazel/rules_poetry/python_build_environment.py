"""Run a Python build module with disposable, absolute home and cache paths."""

import os
import runpy
import sys
import tempfile
from pathlib import Path


def main() -> None:
    module, *arguments = sys.argv[1:]
    action_dir = Path.cwd().resolve()
    with tempfile.TemporaryDirectory(prefix=".python-build-", dir=action_dir) as scratch:
        scratch_dir = Path(scratch)
        home = scratch_dir / "home"
        cache = home / ".cache"
        cargo = home / ".cargo"
        tmp = scratch_dir / "tmp"
        local = home / "AppData" / "Local"
        roaming = home / "AppData" / "Roaming"
        for directory in (home, cache, cargo, tmp, local, roaming):
            directory.mkdir(parents=True, exist_ok=True)
        environment = {
            "HOME": str(home),
            "USERPROFILE": str(home),
            "CARGO_HOME": str(cargo),
            "XDG_CACHE_HOME": str(cache),
            "LOCALAPPDATA": str(local),
            "APPDATA": str(roaming),
            "TMPDIR": str(tmp),
            "TEMP": str(tmp),
            "TMP": str(tmp),
        }
        previous = {name: os.environ.get(name) for name in environment}
        previous_tempdir = tempfile.tempdir
        previous_argv = sys.argv
        try:
            os.environ.update(environment)
            # tempfile can cache the account's temp directory before this launcher runs.
            tempfile.tempdir = str(tmp)
            sys.argv = [module, *arguments]
            runpy.run_module(module, run_name="__main__", alter_sys=True)
        finally:
            os.chdir(action_dir)
            sys.argv = previous_argv
            tempfile.tempdir = previous_tempdir
            for name, value in previous.items():
                if value is None:
                    os.environ.pop(name, None)
                else:
                    os.environ[name] = value


if __name__ == "__main__":
    main()
