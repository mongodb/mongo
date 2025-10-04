"""Package containing subclasses of unittest.TestCase."""

from buildscripts.resmokelib.testing.testcases.interface import make_test_case
from buildscripts.resmokelib.utils import autoloader as _autoloader

__all__ = ["make_test_case"]

# We dynamically load all modules in the testcases/ package so that any TestCase classes declared
# within them are automatically registered.
_autoloader.load_all_modules(name=__name__, path=__path__)  # type: ignore
