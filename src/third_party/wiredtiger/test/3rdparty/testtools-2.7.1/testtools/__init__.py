# Copyright (c) 2008-2012 testtools developers. See LICENSE for details.

"""Extensions to the standard Python unittest library."""

__all__ = [
    'clone_test_with_new_id',
    'CopyStreamResult',
    'ConcurrentTestSuite',
    'ConcurrentStreamTestSuite',
    'DecorateTestCaseResult',
    'ErrorHolder',
    'ExpectedException',
    'ExtendedToOriginalDecorator',
    'ExtendedToStreamDecorator',
    'FixtureSuite',
    'iterate_tests',
    'MultipleExceptions',
    'MultiTestResult',
    'PlaceHolder',
    'run_test_with',
    'ResourcedToStreamDecorator',
    'Tagger',
    'TestCase',
    'TestByTestResult',
    'TestResult',
    'TestResultDecorator',
    'TextTestResult',
    'RunTest',
    'skip',
    'skipIf',
    'skipUnless',
    'StreamFailFast',
    'StreamResult',
    'StreamResultRouter',
    'StreamSummary',
    'StreamTagger',
    'StreamToDict',
    'StreamToExtendedDecorator',
    'StreamToQueue',
    'TestControl',
    'ThreadsafeForwardingResult',
    'TimestampingStreamResult',
    'try_import',
    'unique_text_generator',
    'version',
    '__version__',
    ]

from testtools.helpers import try_import
from testtools.matchers._impl import Matcher
# Shut up, pyflakes. We are importing for documentation, not for namespacing.
Matcher

from testtools.runtest import (
    MultipleExceptions,
    RunTest,
)
from testtools.testcase import (
    DecorateTestCaseResult,
    ErrorHolder,
    ExpectedException,
    PlaceHolder,
    TestCase,
    clone_test_with_new_id,
    run_test_with,
    skip,
    skipIf,
    skipUnless,
    unique_text_generator,
)
from testtools.testresult import (
    CopyStreamResult,
    ExtendedToOriginalDecorator,
    ExtendedToStreamDecorator,
    MultiTestResult,
    ResourcedToStreamDecorator,
    StreamFailFast,
    StreamResult,
    StreamResultRouter,
    StreamSummary,
    StreamTagger,
    StreamToDict,
    StreamToExtendedDecorator,
    StreamToQueue,
    Tagger,
    TestByTestResult,
    TestControl,
    TestResult,
    TestResultDecorator,
    TextTestResult,
    ThreadsafeForwardingResult,
    TimestampingStreamResult,
)
from testtools.testsuite import (
    ConcurrentTestSuite,
    ConcurrentStreamTestSuite,
    FixtureSuite,
    iterate_tests,
)

# same format as sys.version_info: "A tuple containing the five components of
# the version number: major, minor, micro, releaselevel, and serial. All
# values except releaselevel are integers; the release level is 'alpha',
# 'beta', 'candidate', or 'final'. The version_info value corresponding to the
# Python version 2.0 is (2, 0, 0, 'final', 0)."  Additionally we use a
# releaselevel of 'dev' for unreleased under-development code.
#
# If the releaselevel is 'alpha' then the major/minor/micro components are not
# established at this point, and setup.py will use a version of next-$(revno).
# If the releaselevel is 'final', then the tarball will be major.minor.micro.
# Otherwise it is major.minor.micro~$(revno).

try:
    # If setuptools_scm is installed (e.g. in a development environment with
    # an editable install), then use it to determine the version dynamically.
    from setuptools_scm import get_version

    # This will fail with LookupError if the package is not installed in
    # editable mode or if Git is not installed.
    version = get_version(root="..", relative_to=__file__)
    __version__ = tuple(version.split('.'))
except (ImportError, LookupError):
    # As a fallback, use the version that is hard-coded in the file.
    __version__ = (2, 7, 1, 'final', 0)
    # try:
    #     from ._version import (__version__, version)
    # except ModuleNotFoundError:
    #     # The user is probably trying to run this without having installed
    #     # the package, so complain.
    #     raise RuntimeError(
    #         "Testtools is not correctly installed. Please install it with pip.")
