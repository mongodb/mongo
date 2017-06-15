"""
Fixtures for executing JSTests against.
"""

from __future__ import absolute_import

from .interface import Fixture as _Fixture
from .interface import make_fixture
from ...utils import autoloader as _autoloader


NOOP_FIXTURE_CLASS = _Fixture.REGISTERED_NAME


# We dynamically load all modules in the fixtures/ package so that any Fixture classes declared
# within them are automatically registered.
_autoloader.load_all_modules(name=__name__, path=__path__)
