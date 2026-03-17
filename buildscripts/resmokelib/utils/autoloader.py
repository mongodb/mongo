"""Utility for loading all modules within a package."""

import importlib
import os
import pkgutil
import sys
import types


def load_all_modules(name: str, path: list[str]) -> None:
    """Dynamically load all modules in the 'name' package.

    This function is useful in combination with the registry.py module
    so that any classes declared within the package are automatically
    registered.

    The following is the intended usage within the __init__.py file for
    a package:

        from utils import autoloader as _autoloader
        _autoloader.load_all_modules(name=__name__, path=__path__)
    """

    # If package doesn't exist yet, ensure all parent packages are created
    if name not in sys.modules:
        parts = name.split(".")
        for i in range(len(parts)):
            parent = ".".join(parts[: i + 1])
            if parent not in sys.modules:
                # Create a synthetic module for the parent package

                parent_module = types.ModuleType(parent)
                parent_module.__package__ = parent

                # Set __path__ to make it a proper package that can contain submodules
                # Derive parent path by going up the directory tree
                if parent == name:
                    # Target package gets the provided path
                    parent_module.__path__ = path
                else:
                    # Parent packages get paths derived from the target path
                    # Calculate how many levels up we need to go
                    levels_up = len(parts) - (i + 1)
                    parent_paths = []
                    for p in path:
                        # Go up 'levels_up' directories from the target path
                        parent_path = p
                        for _ in range(levels_up):
                            parent_path = os.path.dirname(parent_path)
                        parent_paths.append(parent_path)
                    parent_module.__path__ = parent_paths

                sys.modules[parent] = parent_module

    for _, module, _ in pkgutil.walk_packages(path=path):
        importlib.import_module("." + module, package=name)
