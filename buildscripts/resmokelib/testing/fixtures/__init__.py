"""Fixture for executing JSTests against."""

from buildscripts.resmokelib.testing.fixtures._builder import make_fixture
from buildscripts.resmokelib.testing.fixtures.external import ExternalFixture as _ExternalFixture
from buildscripts.resmokelib.testing.fixtures.interface import NoOpFixture as _NoOpFixture
from buildscripts.resmokelib.utils import autoloader as _autoloader

EXTERNAL_FIXTURE_CLASS = _ExternalFixture.REGISTERED_NAME
NOOP_FIXTURE_CLASS = _NoOpFixture.REGISTERED_NAME

__all__ = ["make_fixture"]

# We dynamically load all modules in the fixtures/ package so that any Fixture classes declared
# within them are automatically registered.
_autoloader.load_all_modules(name=__name__, path=__path__)  # type: ignore
