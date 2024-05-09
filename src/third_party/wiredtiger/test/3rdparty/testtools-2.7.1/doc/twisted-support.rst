.. _twisted-support:

Twisted support
===============

testtools provides support for testing Twisted code.  Install the
``testtools[twisted]`` extra to use this.


Matching Deferreds
------------------

testtools provides support for making assertions about synchronous
:py:class:`~twisted.internet.defer.Deferred`\s.

A "synchronous" :py:class:`~twisted.internet.defer.Deferred` is one that does
not need the reactor or any other asynchronous process in order to fire.

Normal application code can't know when a
:py:class:`~twisted.internet.defer.Deferred` is going to fire, because that is
generally left up to the reactor. Well-written unit tests provide fake
reactors, or don't use the reactor at all, so that
:py:class:`~twisted.internet.defer.Deferred`\s fire synchronously.

These matchers allow you to make assertions about when and how
:py:class:`~twisted.internet.defer.Deferred`\s fire, and about what values
they fire with.

See also `Testing Deferreds without the reactor`_ and the `Deferred howto`_.

.. autofunction:: testtools.twistedsupport.succeeded
   :noindex:

.. autofunction:: testtools.twistedsupport.failed
   :noindex:

.. autofunction:: testtools.twistedsupport.has_no_result
   :noindex:


Running tests in the reactor
----------------------------

testtools provides support for running asynchronous Twisted tests: tests that
return a :py:class:`~twisted.internet.defer.Deferred` and run the reactor
until it fires and its callback chain is completed.

Here's how to use it::

  from testtools import TestCase
  from testtools.twistedsupport import AsynchronousDeferredRunTest

  class MyTwistedTests(TestCase):

      run_tests_with = AsynchronousDeferredRunTest

      def test_foo(self):
          # ...
          return d

Note that you do *not* have to use a special base ``TestCase`` in order to run
Twisted tests, you should just use the regular :py:class:`testtools.TestCase`
base class.

You can also run individual tests within a test case class using the Twisted
test runner::

   class MyTestsSomeOfWhichAreTwisted(TestCase):

       def test_normal(self):
           pass

       @run_test_with(AsynchronousDeferredRunTest)
       def test_twisted(self):
           # ...
           return d

See :py:class:`~testtools.twistedsupport.AsynchronousDeferredRunTest` and
:py:class:`~testtools.twistedsupport.AsynchronousDeferredRunTestForBrokenTwisted`
for more information.


Controlling the Twisted logs
----------------------------

Users of Twisted Trial will be accustomed to all tests logging to
``_trial_temp/test.log``. By default,
:py:class:`~testtools.twistedsupport.AsynchronousDeferredRunTest` will *not*
do this, but will instead:

 1. suppress all messages logged during the test run
 2. attach them as the ``twisted-log`` detail (see :ref:`details`) which is
    shown if the test fails

The first behavior is controlled by the ``suppress_twisted_logging`` parameter
to :py:class:`~testtools.twistedsupport.AsynchronousDeferredRunTest`, which is
set to ``True`` by default. The second is controlled by the
``store_twisted_logs`` parameter, which is also ``True`` by default.

If ``store_twisted_logs`` is set to ``False``, you can still get the logs
attached as a detail by using the
:py:class:`~testtools.twistedsupport.CaptureTwistedLogs` fixture. Using the
:py:class:`~testtools.twistedsupport.CaptureTwistedLogs` fixture is equivalent
to setting ``store_twisted_logs`` to ``True``.

For example::

    class DoNotCaptureLogsTests(TestCase):
        run_tests_with = partial(AsynchronousDeferredRunTest,
                                 store_twisted_logs=False)

        def test_foo(self):
            log.msg('logs from this test are not attached')

        def test_bar(self):
            self.useFixture(CaptureTwistedLogs())
            log.msg('logs from this test *are* attached')


Converting Trial tests to testtools tests
-----------------------------------------

* Use the :py:class:`~testtools.twistedsupport.AsynchronousDeferredRunTest` runner
* Make sure to upcall to :py:meth:`.TestCase.setUp` and
  :py:meth:`.TestCase.tearDown`
* Don't use ``setUpClass`` or ``tearDownClass``
* Don't expect setting ``.todo``, ``.timeout`` or ``.skip`` attributes to do
  anything
* Replace
  :py:meth:`twisted.trial.unittest.SynchronousTestCase.flushLoggedErrors`
  with
  :py:func:`~testtools.twistedsupport.flush_logged_errors`
* Replace :py:meth:`twisted.trial.unittest.TestCase.assertFailure` with
  :py:func:`~testtools.twistedsupport.assert_fails_with`
* Trial spins the reactor a couple of times before cleaning it up,
  :py:class:`~testtools.twistedsupport.AsynchronousDeferredRunTest` does not. If
  you rely on this behavior, use
  :py:class:`~testtools.twistedsupport.AsynchronousDeferredRunTestForBrokenTwisted`.


.. _Deferred Howto: http://twistedmatrix.com/documents/current/core/howto/defer.html
.. _Testing Deferreds without the reactor:
   http://twistedmatrix.com/documents/current/core/howto/trial.html#testing-deferreds-without-the-reactor

