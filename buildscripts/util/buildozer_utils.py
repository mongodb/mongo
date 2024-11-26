import subprocess
from typing import List


def _bd_command(cmd: str, labels: List[str]):
    p = subprocess.run(
        f"buildozer '{cmd}' " + " ".join(labels),
        capture_output=True,
        shell=True,
        text=True,
        check=True,
    )

    return p


def bd_add(labels: List[str], attr: str, values: List[str]) -> None:
    _bd_command(f'add {attr} {" ".join(values)}', labels)


def bd_remove(labels: List[str], attr: str, values: List[str]) -> None:
    _bd_command(f'remove {attr} {" ".join(values)}', labels)


def bd_new(package: str, rule_kind: str, rule_name: str) -> None:
    _bd_command(f"new {rule_kind} {rule_name}", [package])


def bd_comment(labels: List[str], comment: str, attr: str = "", value: str = "") -> None:
    _bd_command(f"comment {attr} {value} {comment}", labels)
