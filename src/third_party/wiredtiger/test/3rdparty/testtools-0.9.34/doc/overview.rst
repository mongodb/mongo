======================================
testtools: tasteful testing for Python
======================================

testtools is a set of extensions to the Python standard library's unit testing
framework. These extensions have been derived from many years of experience
with unit testing in Python and come from many different sources. testtools
supports Python versions all the way back to Python 2.6.

What better way to start than with a contrived code snippet?::

  from testtools import TestCase
  from testtools.content import Content
  from testtools.content_type import UTF8_TEXT
  from testtools.matchers import Equals

  from myproject import SillySquareServer

  class TestSillySquareServer(TestCase):

      def setUp(self):
          super(TestSillySquare, self).setUp()
          self.server = self.useFixture(SillySquareServer())
          self.addCleanup(self.attach_log_file)

      def attach_log_file(self):
          self.addDetail(
              'log-file',
              Content(UTF8_TEXT
                      lambda: open(self.server.logfile, 'r').readlines()))

      def test_server_is_cool(self):
          self.assertThat(self.server.temperature, Equals("cool"))

      def test_square(self):
          self.assertThat(self.server.silly_square_of(7), Equals(49))


Why use testtools?
==================

Better assertion methods
------------------------

The standard assertion methods that come with unittest aren't as helpful as
they could be, and there aren't quite enough of them.  testtools adds
``assertIn``, ``assertIs``, ``assertIsInstance`` and their negatives.


Matchers: better than assertion methods
---------------------------------------

Of course, in any serious project you want to be able to have assertions that
are specific to that project and the particular problem that it is addressing.
Rather than forcing you to define your own assertion methods and maintain your
own inheritance hierarchy of ``TestCase`` classes, testtools lets you write
your own "matchers", custom predicates that can be plugged into a unit test::

  def test_response_has_bold(self):
     # The response has bold text.
     response = self.server.getResponse()
     self.assertThat(response, HTMLContains(Tag('bold', 'b')))


More debugging info, when you need it
--------------------------------------

testtools makes it easy to add arbitrary data to your test result.  If you
want to know what's in a log file when a test fails, or what the load was on
the computer when a test started, or what files were open, you can add that
information with ``TestCase.addDetail``, and it will appear in the test
results if that test fails.


Extend unittest, but stay compatible and re-usable
--------------------------------------------------

testtools goes to great lengths to allow serious test authors and test
*framework* authors to do whatever they like with their tests and their
extensions while staying compatible with the standard library's unittest.

testtools has completely parametrized how exceptions raised in tests are
mapped to ``TestResult`` methods and how tests are actually executed (ever
wanted ``tearDown`` to be called regardless of whether ``setUp`` succeeds?)

It also provides many simple but handy utilities, like the ability to clone a
test, a ``MultiTestResult`` object that lets many result objects get the
results from one test suite, adapters to bring legacy ``TestResult`` objects
into our new golden age.


Cross-Python compatibility
--------------------------

testtools gives you the very latest in unit testing technology in a way that
will work with Python 2.6, 2.7, 3.1 and 3.2.

If you wish to use testtools with Python 2.4 or 2.5, then please use testtools
0.9.15. Up to then we supported Python 2.4 and 2.5, but we found the
constraints involved in not using the newer language features onerous as we
added more support for versions post Python 3.
