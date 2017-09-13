============================
testtools for framework folk
============================

Introduction
============

In addition to having many features :doc:`for test authors
<for-test-authors>`, testtools also has many bits and pieces that are useful
for folk who write testing frameworks.

If you are the author of a test runner, are working on a very large
unit-tested project, are trying to get one testing framework to play nicely
with another or are hacking away at getting your test suite to run in parallel
over a heterogenous cluster of machines, this guide is for you.

This manual is a summary.  You can get details by consulting the `testtools
API docs`_.


Extensions to TestCase
======================

In addition to the ``TestCase`` specific methods, we have extensions for
``TestSuite`` that also apply to ``TestCase`` (because ``TestCase`` and
``TestSuite`` follow the Composite pattern).

Custom exception handling
-------------------------

testtools provides a way to control how test exceptions are handled.  To do
this, add a new exception to ``self.exception_handlers`` on a
``testtools.TestCase``.  For example::

    >>> self.exception_handlers.insert(-1, (ExceptionClass, handler)).

Having done this, if any of ``setUp``, ``tearDown``, or the test method raise
``ExceptionClass``, ``handler`` will be called with the test case, test result
and the raised exception.

Use this if you want to add a new kind of test result, that is, if you think
that ``addError``, ``addFailure`` and so forth are not enough for your needs.


Controlling test execution
--------------------------

If you want to control more than just how exceptions are raised, you can
provide a custom ``RunTest`` to a ``TestCase``.  The ``RunTest`` object can
change everything about how the test executes.

To work with ``testtools.TestCase``, a ``RunTest`` must have a factory that
takes a test and an optional list of exception handlers.  Instances returned
by the factory must have a ``run()`` method that takes an optional ``TestResult``
object.

The default is ``testtools.runtest.RunTest``, which calls ``setUp``, the test
method, ``tearDown`` and clean ups (see :ref:`addCleanup`) in the normal, vanilla
way that Python's standard unittest_ does.

To specify a ``RunTest`` for all the tests in a ``TestCase`` class, do something
like this::

  class SomeTests(TestCase):
      run_tests_with = CustomRunTestFactory

To specify a ``RunTest`` for a specific test in a ``TestCase`` class, do::

  class SomeTests(TestCase):
      @run_test_with(CustomRunTestFactory, extra_arg=42, foo='whatever')
      def test_something(self):
          pass

In addition, either of these can be overridden by passing a factory in to the
``TestCase`` constructor with the optional ``runTest`` argument.


Test renaming
-------------

``testtools.clone_test_with_new_id`` is a function to copy a test case
instance to one with a new name.  This is helpful for implementing test
parameterization.

.. _force_failure:

Delayed Test Failure
--------------------

Setting the ``testtools.TestCase.force_failure`` instance variable to True will
cause ``testtools.RunTest`` to fail the test case after the test has finished.
This is useful when you want to cause a test to fail, but don't want to
prevent the remainder of the test code from being executed.

Test placeholders
=================

Sometimes, it's useful to be able to add things to a test suite that are not
actually tests.  For example, you might wish to represents import failures
that occur during test discovery as tests, so that your test result object
doesn't have to do special work to handle them nicely.

testtools provides two such objects, called "placeholders": ``PlaceHolder``
and ``ErrorHolder``.  ``PlaceHolder`` takes a test id and an optional
description.  When it's run, it succeeds.  ``ErrorHolder`` takes a test id,
and error and an optional short description.  When it's run, it reports that
error.

These placeholders are best used to log events that occur outside the test
suite proper, but are still very relevant to its results.

e.g.::

  >>> suite = TestSuite()
  >>> suite.add(PlaceHolder('I record an event'))
  >>> suite.run(TextTestResult(verbose=True))
  I record an event                                                   [OK]


Test instance decorators
========================

DecorateTestCaseResult
----------------------

This object calls out to your code when ``run`` / ``__call__`` are called and
allows the result object that will be used to run the test to be altered. This
is very useful when working with a test runner that doesn't know your test case
requirements. For instance, it can be used to inject a ``unittest2`` compatible
adapter when someone attempts to run your test suite with a ``TestResult`` that
does not support ``addSkip`` or other ``unittest2`` methods. Similarly it can
aid the migration to ``StreamResult``.

e.g.::

 >>> suite = TestSuite()
 >>> suite = DecorateTestCaseResult(suite, ExtendedToOriginalDecorator)

Extensions to TestResult
========================

StreamResult
------------

``StreamResult`` is a new API for dealing with test case progress that supports
concurrent and distributed testing without the various issues that
``TestResult`` has such as buffering in multiplexers.

The design has several key principles:

* Nothing that requires up-front knowledge of all tests.

* Deal with tests running in concurrent environments, potentially distributed
  across multiple processes (or even machines). This implies allowing multiple
  tests to be active at once, supplying time explicitly, being able to
  differentiate between tests running in different contexts and removing any
  assumption that tests are necessarily in the same process.

* Make the API as simple as possible - each aspect should do one thing well.

The ``TestResult`` API this is intended to replace has three different clients.

* Each executing ``TestCase`` notifies the ``TestResult`` about activity.

* The testrunner running tests uses the API to find out whether the test run
  had errors, how many tests ran and so on.

* Finally, each ``TestCase`` queries the ``TestResult`` to see whether the test
  run should be aborted.

With ``StreamResult`` we need to be able to provide a ``TestResult`` compatible
adapter (``StreamToExtendedDecorator``) to allow incremental migration.
However, we don't need to conflate things long term. So - we define three
separate APIs, and merely mix them together to provide the
``StreamToExtendedDecorator``. ``StreamResult`` is the first of these APIs -
meeting the needs of ``TestCase`` clients. It handles events generated by
running tests. See the API documentation for ``testtools.StreamResult`` for
details.

StreamSummary
-------------

Secondly we define the ``StreamSummary`` API which takes responsibility for
collating errors, detecting incomplete tests and counting tests. This provides
a compatible API with those aspects of ``TestResult``. Again, see the API
documentation for ``testtools.StreamSummary``.

TestControl
-----------

Lastly we define the ``TestControl`` API which is used to provide the
``shouldStop`` and ``stop`` elements from ``TestResult``. Again, see the API
documentation for ``testtools.TestControl``. ``TestControl`` can be paired with
a ``StreamFailFast`` to trigger aborting a test run when a failure is observed.
Aborting multiple workers in a distributed environment requires hooking
whatever signalling mechanism the distributed environment has up to a
``TestControl`` in each worker process.

StreamTagger
------------

A ``StreamResult`` filter that adds or removes tags from events::

    >>> from testtools import StreamTagger
    >>> sink = StreamResult()
    >>> result = StreamTagger([sink], set(['add']), set(['discard']))
    >>> result.startTestRun()
    >>> # Run tests against result here.
    >>> result.stopTestRun()

StreamToDict
------------

A simplified API for dealing with ``StreamResult`` streams. Each test is
buffered until it completes and then reported as a trivial dict. This makes
writing analysers very easy - you can ignore all the plumbing and just work
with the result. e.g.::

    >>> from testtools import StreamToDict
    >>> def handle_test(test_dict):
    ...     print(test_dict['id'])
    >>> result = StreamToDict(handle_test)
    >>> result.startTestRun()
    >>> # Run tests against result here.
    >>> # At stopTestRun() any incomplete buffered tests are announced.
    >>> result.stopTestRun()

ExtendedToStreamDecorator
-------------------------

This is a hybrid object that combines both the ``Extended`` and ``Stream``
``TestResult`` APIs into one class, but only emits ``StreamResult`` events.
This is useful when a ``StreamResult`` stream is desired, but you cannot
be sure that the tests which will run have been updated to the ``StreamResult``
API.

StreamToExtendedDecorator
-------------------------

This is a simple converter that emits the ``ExtendedTestResult`` API in
response to events from the ``StreamResult`` API. Useful when outputting
``StreamResult`` events from a ``TestCase`` but the supplied ``TestResult``
does not support the ``status`` and ``file`` methods.

StreamToQueue
-------------

This is a ``StreamResult`` decorator for reporting tests from multiple threads
at once. Each method submits an event to a supplied Queue object as a simple
dict. See ``ConcurrentStreamTestSuite`` for a convenient way to use this.

TimestampingStreamResult
------------------------

This is a ``StreamResult`` decorator for adding timestamps to events that lack
them. This allows writing the simplest possible generators of events and
passing the events via this decorator to get timestamped data. As long as
no buffering/queueing or blocking happen before the timestamper sees the event
the timestamp will be as accurate as if the original event had it.

StreamResultRouter
------------------

This is a ``StreamResult`` which forwards events to an arbitrary set of target
``StreamResult`` objects. Events that have no forwarding rule are passed onto
an fallback ``StreamResult`` for processing. The mapping can be changed at
runtime, allowing great flexibility and responsiveness to changes. Because
The mapping can change dynamically and there could be the same recipient for
two different maps, ``startTestRun`` and ``stopTestRun`` handling is fine
grained and up to the user.

If no fallback has been supplied, an unroutable event will raise an exception.

For instance::

    >>> router = StreamResultRouter()
    >>> sink = doubles.StreamResult()
    >>> router.add_rule(sink, 'route_code_prefix', route_prefix='0',
    ...     consume_route=True)
    >>> router.status(test_id='foo', route_code='0/1', test_status='uxsuccess')

Would remove the ``0/`` from the route_code and forward the event like so::

    >>> sink.status('test_id=foo', route_code='1', test_status='uxsuccess')

See ``pydoc testtools.StreamResultRouter`` for details.

TestResult.addSkip
------------------

This method is called on result objects when a test skips. The
``testtools.TestResult`` class records skips in its ``skip_reasons`` instance
dict. The can be reported on in much the same way as succesful tests.


TestResult.time
---------------

This method controls the time used by a ``TestResult``, permitting accurate
timing of test results gathered on different machines or in different threads.
See pydoc testtools.TestResult.time for more details.


ThreadsafeForwardingResult
--------------------------

A ``TestResult`` which forwards activity to another test result, but synchronises
on a semaphore to ensure that all the activity for a single test arrives in a
batch. This allows simple TestResults which do not expect concurrent test
reporting to be fed the activity from multiple test threads, or processes.

Note that when you provide multiple errors for a single test, the target sees
each error as a distinct complete test.


MultiTestResult
---------------

A test result that dispatches its events to many test results.  Use this
to combine multiple different test result objects into one test result object
that can be passed to ``TestCase.run()`` or similar.  For example::

  a = TestResult()
  b = TestResult()
  combined = MultiTestResult(a, b)
  combined.startTestRun()  # Calls a.startTestRun() and b.startTestRun()

Each of the methods on ``MultiTestResult`` will return a tuple of whatever the
component test results return.


TestResultDecorator
-------------------

Not strictly a ``TestResult``, but something that implements the extended
``TestResult`` interface of testtools.  It can be subclassed to create objects
that wrap ``TestResults``.


TextTestResult
--------------

A ``TestResult`` that provides a text UI very similar to the Python standard
library UI. Key differences are that its supports the extended outcomes and
details API, and is completely encapsulated into the result object, permitting
it to be used without a 'TestRunner' object. Not all the Python 2.7 outcomes
are displayed (yet). It is also a 'quiet' result with no dots or verbose mode.
These limitations will be corrected soon.


ExtendedToOriginalDecorator
---------------------------

Adapts legacy ``TestResult`` objects, such as those found in older Pythons, to
meet the testtools ``TestResult`` API.


Test Doubles
------------

In testtools.testresult.doubles there are three test doubles that testtools
uses for its own testing: ``Python26TestResult``, ``Python27TestResult``,
``ExtendedTestResult``. These TestResult objects implement a single variation of
the TestResult API each, and log activity to a list ``self._events``. These are
made available for the convenience of people writing their own extensions.


startTestRun and stopTestRun
----------------------------

Python 2.7 added hooks ``startTestRun`` and ``stopTestRun`` which are called
before and after the entire test run. 'stopTestRun' is particularly useful for
test results that wish to produce summary output.

``testtools.TestResult`` provides default ``startTestRun`` and ``stopTestRun``
methods, and he default testtools runner will call these methods
appropriately.

The ``startTestRun`` method will reset any errors, failures and so forth on
the result, making the result object look as if no tests have been run.


Extensions to TestSuite
=======================

ConcurrentTestSuite
-------------------

A TestSuite for parallel testing. This is used in conjuction with a helper that
runs a single suite in some parallel fashion (for instance, forking, handing
off to a subprocess, to a compute cloud, or simple threads).
ConcurrentTestSuite uses the helper to get a number of separate runnable
objects with a run(result), runs them all in threads using the
ThreadsafeForwardingResult to coalesce their activity.

ConcurrentStreamTestSuite
-------------------------

A variant of ConcurrentTestSuite that uses the new StreamResult API instead of
the TestResult API. ConcurrentStreamTestSuite coordinates running some number
of test/suites concurrently, with one StreamToQueue per test/suite.

Each test/suite gets given its own ExtendedToStreamDecorator +
TimestampingStreamResult wrapped StreamToQueue instance, forwarding onto the
StreamResult that ConcurrentStreamTestSuite.run was called with.

ConcurrentStreamTestSuite is a thin shim and it is easy to implement your own
specialised form if that is needed.

FixtureSuite
------------

A test suite that sets up a fixture_ before running any tests, and then tears
it down after all of the tests are run. The fixture is *not* made available to
any of the tests due to there being no standard channel for suites to pass
information to the tests they contain (and we don't have enough data on what
such a channel would need to achieve to design a good one yet - or even decide
if it is a good idea).

sorted_tests
------------

Given the composite structure of TestSuite / TestCase, sorting tests is
problematic - you can't tell what functionality is embedded into custom Suite
implementations. In order to deliver consistent test orders when using test
discovery (see http://bugs.python.org/issue16709), testtools flattens and
sorts tests that have the standard TestSuite, and defines a new method
sort_tests, which can be used by non-standard TestSuites to know when they
should sort their tests. An example implementation can be seen at
``FixtureSuite.sorted_tests``.

If there are duplicate test ids in a suite, ValueError will be raised.

filter_by_ids
-------------

Similarly to ``sorted_tests`` running a subset of tests is problematic - the
standard run interface provides no way to limit what runs. Rather than
confounding the two problems (selection and execution) we defined a method
that filters the tests in a suite (or a case) by their unique test id.
If you a writing custom wrapping suites, consider implementing filter_by_ids
to support this (though most wrappers that subclass ``unittest.TestSuite`` will
work just fine [see ``testtools.testsuite.filter_by_ids`` for details.]

Extensions to TestRunner
========================

To facilitate custom listing of tests, ``testtools.run.TestProgram`` attempts
to call ``list`` on the ``TestRunner``, falling back to a generic
implementation if it is not present.

.. _`testtools API docs`: http://mumak.net/testtools/apidocs/
.. _unittest: http://docs.python.org/library/unittest.html
.. _fixture: http://pypi.python.org/pypi/fixtures
