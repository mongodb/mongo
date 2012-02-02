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


Extensions to TestResult
========================

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

FixtureSuite
------------

A test suite that sets up a fixture_ before running any tests, and then tears
it down after all of the tests are run. The fixture is *not* made available to
any of the tests.

.. _`testtools API docs`: http://mumak.net/testtools/apidocs/
.. _unittest: http://docs.python.org/library/unittest.html
.. _fixture: http://pypi.python.org/pypi/fixtures
