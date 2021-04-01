"""Convert Evergreen's expansions.yml to an eval-able shell script."""
import sys
import platform
from shlex import quote
from typing import Any

import yaml
import click


def _error(msg: str) -> None:
    print(f"___expansions_error={quote(msg)}")
    sys.exit(1)


@click.command()
@click.argument("expansions_file", type=str)
@click.argument("defaults_file", type=str)
def _main(expansions_file, defaults_file):
    try:
        with open(defaults_file) as fh:
            defaults = yaml.safe_load(fh)
        with open(expansions_file) as fh:
            expansions = yaml.safe_load(fh)

        # inject defaults into expansions
        for key, value in defaults.items():
            if key not in expansions:
                expansions[key] = str(value)

        if not isinstance(expansions, dict):
            _error("ERROR: expected to read a dictionary. Has the output format "
                   "of expansions.write changed?")

        if not expansions:
            _error("ERROR: found 0 expansions. This is almost certainly wrong.")

        for key, value in expansions.items():
            print(f"{key}={quote(value)}; ", end="")

    except Exception as ex:  # pylint: disable=broad-except
        _error(ex)


if __name__ == "__main__":
    _main()  # pylint: disable=no-value-for-parameter
