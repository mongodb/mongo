import subprocess
from typing import List


def _bd_command(cmd: str, labels: List[str]):
    print(
        f"buildozer '{cmd}' " + " ".join(labels),
    )
    p = subprocess.run(
        f"buildozer '{cmd}' " + " ".join(labels),
        capture_output=True,
        shell=True,
        text=True,
        check=False,
    )
    if p.returncode != 0:
        print(p.stdout, p.stderr)
        raise Exception()
    return p


def bd_add(labels: List[str], attr: str, values: List[str]) -> None:
    _bd_command(f'add {attr} {" ".join(values)}', labels)


def bd_set(labels: List[str], attr: str, value: str) -> None:
    _bd_command(f"set {attr} {value}", labels)


def bd_remove(labels: List[str], attr: str, values: List[str]) -> None:
    _bd_command(f'remove {attr} {" ".join(values)}', labels)


def bd_new(package: str, rule_kind: str, rule_name: str) -> None:
    _bd_command(f"new {rule_kind} {rule_name}", [package])


def bd_comment(labels: List[str], comment: str, attr: str = "", value: str = "") -> None:
    _bd_command(f"comment {attr} {value} {comment}", labels)


def bd_print(labels: List[str], attrs: List[str]) -> None:
    _bd_command(f'print {" ".join(attrs)}', labels)


def bd_new_load(packages: List[str], path: str, rules: List[str]) -> None:
    _bd_command(f'new_load {path} {" ".join(rules)}', packages)


def bd_fix(fixes: List[str]) -> None:
    _bd_command(f'fix {" ".join(fixes)}', [])


def bd_copy(labels: List[str], attr: str, from_rule: str) -> None:
    _bd_command(f"copy {attr} {from_rule}", labels)
