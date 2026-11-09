"""Shell selection for tests that execute Evergreen's Cygwin scripts."""

import shutil
import sys
from pathlib import Path


def bash_executable() -> str:
    """Find Cygwin Bash on Windows, where bare bash may launch WSL."""
    if sys.platform == "win32":
        cygpath = shutil.which("cygpath")
        candidates = []
        if cygpath:
            candidates.append(Path(cygpath).with_name("bash.exe"))
        candidates.extend((Path("C:/cygwin64/bin/bash.exe"), Path("C:/cygwin/bin/bash.exe")))
        for candidate in candidates:
            if candidate.is_file():
                return str(candidate)
        raise FileNotFoundError("Cygwin Bash is required for Evergreen shell tests")
    bash = shutil.which("bash")
    if bash is None:
        raise FileNotFoundError("Bash is required for Evergreen shell tests")
    return bash
