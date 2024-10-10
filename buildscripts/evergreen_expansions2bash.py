"""Convert Evergreen's expansions.yml to an eval-able shell script."""

import sys
from shlex import quote


def _error(msg: str) -> None:
    print(f"___expansions_error={quote(msg)}")
    sys.exit(1)


try:
    import click
    import yaml
except ModuleNotFoundError:
    _error(
        "ERROR: Failed to import a dependency. This is almost certainly because "
        "the task did not initialize the venv immediately after cloning the repository."
    )


def _load_defaults(defaults_file: str) -> dict:
    with open(defaults_file) as fh:
        defaults = yaml.safe_load(fh)
        if not isinstance(defaults, dict):
            _error(
                "ERROR: expected to read a dictionary. expansions.defaults.yml"
                "must be a dictionary. Check the indentation."
            )

        # expansions MUST be strings. Reject any that are not
        bad_expansions = set()
        for key, value in defaults.items():
            if not isinstance(value, str):
                bad_expansions.add(key)

        if bad_expansions:
            _error(
                "ERROR: all default expansions must be strings. You can "
                " fix this error by quoting the values in expansions.defaults.yml. "
                "Integers, floating points, 'true', 'false', and 'null' "
                "must be quoted. The following keys were interpreted as "
                f"other types: {bad_expansions}"
            )

        # These values show up if 1. Python's str is used to naively convert
        # a boolean to str, 2. A human manually entered one of those strings.
        # Either way, our shell scripts expect 'true' or 'false' (leading
        # lowercase), and we reject them as errors. This will probably save
        # someone a lot of time, but if this assumption proves wrong, start
        # a conversation in #server-testing.
        risky_boolean_keys = set()
        for key, value in defaults.items():
            if value in ("True", "False"):
                risky_boolean_keys.add(key)

        if risky_boolean_keys:
            _error(
                "ERROR: Found keys which had 'True' or 'False' as values. "
                "Shell scripts assume that booleans are represented as 'true'"
                " or 'false' (leading lowercase). If you added a new boolean, "
                "ensure that it's represented in lowercase. If not, please report this in "
                f"#server-testing. Keys with bad values: {risky_boolean_keys}"
            )

        return defaults


def _load_expansions(expansions_file) -> dict:
    with open(expansions_file) as fh:
        expansions = yaml.safe_load(fh)

        if not isinstance(expansions, dict):
            _error(
                "ERROR: expected to read a dictionary. Has the output format "
                "of expansions.write changed?"
            )

        if not expansions:
            _error("ERROR: found 0 expansions. This is almost certainly wrong.")

        return expansions


def _clean_key(key):
    return key.replace("-", "_")


@click.command()
@click.argument("expansions_file", type=str)
@click.argument("defaults_file", type=str)
def _main(expansions_file: str, defaults_file: str):
    try:
        defaults = _load_defaults(defaults_file)
        expansions = _load_expansions(expansions_file)

        # inject defaults into expansions
        for key, value in defaults.items():
            if key not in expansions:
                expansions[key] = value

        for key, value in expansions.items():
            print(f"{_clean_key(key)}={quote(value)}; ", end="")

    except Exception as ex:  # pylint: disable=broad-except
        _error(ex)


if __name__ == "__main__":
    _main()  # pylint: disable=no-value-for-parameter
