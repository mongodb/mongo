"""Python utilities around evergreen expansions."""

import os
from functools import cache
from pathlib import Path
from typing import Any

import yaml


@cache
def get_expansions() -> dict:
    current_path = Path(__file__).resolve()
    evergreen_workdir = current_path.parents[3]
    expansions_file = os.path.join(evergreen_workdir, "expansions.yml")
    if not os.path.exists(expansions_file):
        return None

    with open(expansions_file, "r", encoding="utf8") as file:
        return yaml.safe_load(file)


def get_expansion(key: str, default: Any = None) -> Any:
    expansions = get_expansions()
    if expansions is None:
        return default
    return expansions.get(key, default)
