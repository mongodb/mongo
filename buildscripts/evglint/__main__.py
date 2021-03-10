"""entry point for evglint."""
import os
import copy
import sys
import itertools
from typing import Callable, List, Iterator, Dict

import click
from typing_extensions import TypedDict
if not __package__:
    newpath = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    sys.path.append(newpath)

# pylint: disable=wrong-import-position
from buildscripts.evglint.rules import RULES
from buildscripts.evglint.model import LintError, LintRule
from buildscripts.evglint.yamlhandler import load_file

_LINTABLE_YAMLS: List[str] = [
    "evergreen.yml"
    # :(
    # "perf.yml"
    # "system-perf.yml"
]


@click.command()
def _main() -> int:
    ret = 0
    for yaml_file in _LINTABLE_YAMLS:
        yaml_dict = load_file(yaml_file)
        errors: Dict[str, List[LintError]] = {}
        for rule, fn in RULES.items():
            rule_errors = fn(yaml_dict)
            if rule_errors:
                errors[rule] = rule_errors

        err_count = 0
        for error_list in errors.values():
            err_count += len(error_list)

        if err_count:
            print(f"{err_count} errors found in '{yaml_file}':")
            print_nl = False
            for rule, error_list in errors.items():
                for error in error_list:
                    if print_nl:
                        print("")
                    print(f"{rule}:", error)
                    print_nl = True
            ret = 1

    sys.exit(ret)


if __name__ == "__main__":
    _main()
