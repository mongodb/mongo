"""Python utilities around evergreen expansions."""

import os
from functools import cache
from pathlib import Path
from typing import Any

import yaml


@cache
def get_expansions() -> dict:
    """Return the contents of expansions.yml as a dictionary."""
    current_path = Path(__file__).resolve()
    evergreen_workdir = current_path.parents[3]
    expansions_file = os.path.join(evergreen_workdir, "expansions.yml")
    if not os.path.exists(expansions_file):
        return None

    with open(expansions_file, "r") as file:
        return yaml.safe_load(file)


def get_expansion(key: str, default: Any = None) -> Any:
    """Return the value of the given key from the expansions.yml file."""
    expansions = get_expansions()
    if expansions is None:
        return default
    return expansions.get(key, default)
