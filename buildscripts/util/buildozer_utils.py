import subprocess
from typing import List


def _bd_command(cmd: str, labels: List[str]):
    return subprocess.run(
        ["buildozer"] + [cmd],
        capture_output=True,
        text=True,
    )


def bd_add(labels: List[str], attr: str, values: List[str]) -> None:
    _bd_command(f'add {attr} {"".join(values)}', labels)


def bd_remove(labels: List[str], attr: str, values: List[str]) -> None:
    _bd_command(f'remove {attr} {"".join(values)}', labels)
