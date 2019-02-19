"""Fixture for executing JSTests against."""

from .external import ExternalFixture as _ExternalFixture
from .interface import NoOpFixture as _NoOpFixture
from .interface import make_fixture
from ...utils import autoloader as _autoloader

EXTERNAL_FIXTURE_CLASS = _ExternalFixture.REGISTERED_NAME
NOOP_FIXTURE_CLASS = _NoOpFixture.REGISTERED_NAME

# We dynamically load all modules in the fixtures/ package so that any Fixture classes declared
# within them are automatically registered.
_autoloader.load_all_modules(name=__name__, path=__path__)  # type: ignore
