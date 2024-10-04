"""Testing hooks package.

Package containing classes to customize the behavior of a test fixture
by allowing special code to be executed before or after each test, and
before or after each suite.
"""

from buildscripts.resmokelib.testing.hooks.interface import make_hook
from buildscripts.resmokelib.utils import autoloader as _autoloader

__all__ = ["make_hook"]

# We dynamically load all modules in the hooks/ package so that any Hook classes declared
# within them are automatically registered.
_autoloader.load_all_modules(name=__name__, path=__path__)  # type: ignore
