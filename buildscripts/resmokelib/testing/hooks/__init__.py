"""
Package containing classes to customize the behavior of a test fixture
by allowing special code to be executed before or after each test, and
before or after each suite.
"""

from __future__ import absolute_import

from .interface import make_custom_behavior
from ...utils import autoloader as _autoloader


# We dynamically load all modules in the hooks/ package so that any CustomBehavior classes declared
# within them are automatically registered.
_autoloader.load_all_modules(name=__name__, path=__path__)
