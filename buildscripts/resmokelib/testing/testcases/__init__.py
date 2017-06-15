"""
Package containing subclasses of unittest.TestCase.
"""

from __future__ import absolute_import

from .interface import make_test_case
from ...utils import autoloader as _autoloader


# We dynamically load all modules in the testcases/ package so that any TestCase classes declared
# within them are automatically registered.
_autoloader.load_all_modules(name=__name__, path=__path__)
