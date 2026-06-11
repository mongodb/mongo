import subprocess


class BuildozerRuleNotFoundError(Exception):
    """Raised when buildozer cannot locate a named rule, e.g. rules defined via list comprehensions."""

    pass


def _bd_command(cmd: str, labels: list[str]):
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
        if "not found" in p.stderr:
            raise BuildozerRuleNotFoundError(p.stderr.strip())
        raise Exception()
    return p


def bd_add(labels: list[str], attr: str, values: list[str]) -> None:
    _bd_command(f'add {attr} {" ".join(values)}', labels)


def bd_set(labels: list[str], attr: str, value: str) -> None:
    _bd_command(f"set {attr} {value}", labels)


def bd_remove(labels: list[str], attr: str, values: list[str]) -> None:
    _bd_command(f'remove {attr} {" ".join(values)}', labels)


def bd_new(package: str, rule_kind: str, rule_name: str) -> None:
    _bd_command(f"new {rule_kind} {rule_name}", [package])


def bd_comment(labels: list[str], comment: str, attr: str = "", value: str = "") -> None:
    _bd_command(f"comment {attr} {value} {comment}", labels)


def bd_print(labels: list[str], attrs: list[str]) -> str:
    p = _bd_command(f'print {" ".join(attrs)}', labels)
    return p.stdout


def bd_new_load(packages: list[str], path: str, rules: list[str]) -> None:
    _bd_command(f'new_load {path} {" ".join(rules)}', packages)


def bd_fix(fixes: list[str]) -> None:
    _bd_command(f'fix {" ".join(fixes)}', [])


def bd_copy(labels: list[str], attr: str, from_rule: str) -> None:
    _bd_command(f"copy {attr} {from_rule}", labels)


def bd_move(labels: list[str], old_attr: str, new_attr: str) -> None:
    _bd_command(f"move {old_attr} {new_attr} *", labels)
