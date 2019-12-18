"""Utility for loading all modules within a package."""

import importlib
import pkgutil


def load_all_modules(name, path):
    """Dynamically load all modules in the 'name' package.

    This function is useful in combination with the registry.py module
    so that any classes declared within the package are automatically
    registered.

    The following is the intended usage within the __init__.py file for
    a package:

        from utils import autoloader as _autoloader
        _autoloader.load_all_modules(name=__name__, path=__path__)
    """

    for (_, module, _) in pkgutil.walk_packages(path=path):
        importlib.import_module("." + module, package=name)
