"""Yaml handling helpers for evglint."""
import io
import os
from typing import Union
import yaml


class ReadOnlyDict(dict):
    """RO dictionary wrapper to prevent modifications to the yaml dict."""

    # pylint: disable=no-self-use
    def __readonly__(self, *args, **kwargs):
        raise RuntimeError("Rules must not modify the yaml dictionary")

    __setitem__ = __readonly__
    __delitem__ = __readonly__
    pop = __readonly__
    popitem = __readonly__
    clear = __readonly__
    update = __readonly__
    setdefault = __readonly__
    del __readonly__


def _etc_dir() -> str:
    self_dir = os.path.dirname(os.path.realpath(__file__))
    return os.path.abspath(os.path.join(self_dir, "..", "..", "etc"))


def load_file(yaml_file: Union[str, os.PathLike]) -> dict:
    """Load yaml from a file on disk."""
    with open(os.path.join(_etc_dir(), yaml_file)) as fh:
        return load(fh)


def load(data: Union[io.TextIOWrapper, str, bytes]) -> dict:
    """Given a file handle or buffer, load yaml."""
    yaml_dict = yaml.safe_load(data)
    return ReadOnlyDict(yaml_dict)
