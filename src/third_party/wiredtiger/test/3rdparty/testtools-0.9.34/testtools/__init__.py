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
    'Tagger',
    'TestCase',
    'TestCommand',
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
    'try_imports',
    ]

# Compat - removal announced in 0.9.25.
try:
    from extras import (
        try_import,
        try_imports,
        )
except ImportError:
    # Support reading __init__ for __version__ without extras, because pip does
    # not support setup_requires.
    pass
else:

    from testtools.matchers._impl import (
        Matcher,
        )
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
        )
    from testtools.testresult import (
        CopyStreamResult,
        ExtendedToOriginalDecorator,
        ExtendedToStreamDecorator,
        MultiTestResult,
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
    from testtools.distutilscmd import (
        TestCommand,
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

__version__ = (0, 9, 34, 'final', 0)
