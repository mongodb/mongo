==========================
testtools for test authors
==========================

If you are writing tests for a Python project and you (rather wisely) want to
use testtools to do so, this is the manual for you.

We assume that you already know Python and that you know something about
automated testing already.

If you are a test author of an unusually large or unusually unusual test
suite, you might be interested in :doc:`for-framework-folk`.

You might also be interested in the `testtools API docs`_.


Introduction
============

testtools is a set of extensions to Python's standard unittest module.
Writing tests with testtools is very much like writing tests with standard
Python, or with Twisted's "trial_", or nose_, except a little bit easier and
more enjoyable.

Below, we'll try to give some examples of how to use testtools in its most
basic way, as well as a sort of feature-by-feature breakdown of the cool bits
that you could easily miss.


The basics
==========

Here's what a basic testtools unit tests look like::

  from testtools import TestCase
  from myproject import silly

  class TestSillySquare(TestCase):
      """Tests for silly square function."""

      def test_square(self):
          # 'square' takes a number and multiplies it by itself.
          result = silly.square(7)
          self.assertEqual(result, 49)

      def test_square_bad_input(self):
          # 'square' raises a TypeError if it's given bad input, say a
          # string.
          self.assertRaises(TypeError, silly.square, "orange")


Here you have a class that inherits from ``testtools.TestCase`` and bundles
together a bunch of related tests.  The tests themselves are methods on that
class that begin with ``test_``.

Running your tests
------------------

You can run these tests in many ways.  testtools provides a very basic
mechanism for doing so::

  $ python -m testtools.run exampletest
  Tests running...
  Ran 2 tests in 0.000s

  OK

where 'exampletest' is a module that contains unit tests.  By default,
``testtools.run`` will *not* recursively search the module or package for unit
tests.  To do this, you will need to either have the discover_ module
installed or have Python 2.7 or later, and then run::

  $ python -m testtools.run discover packagecontainingtests

For more information see the Python 2.7 unittest documentation, or::

    python -m testtools.run --help

As your testing needs grow and evolve, you will probably want to use a more
sophisticated test runner.  There are many of these for Python, and almost all
of them will happily run testtools tests.  In particular:

* testrepository_
* Trial_
* nose_
* unittest2_
* `zope.testrunner`_ (aka zope.testing)

From now on, we'll assume that you know how to run your tests.

Running test with Distutils
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

If you are using Distutils_ to build your Python project, you can use the testtools
Distutils_ command to integrate testtools into your Distutils_ workflow::

  from distutils.core import setup
  from testtools import TestCommand
  setup(name='foo',
      version='1.0',
      py_modules=['foo'],
      cmdclass={'test': TestCommand}
  )

You can then run::

  $ python setup.py test -m exampletest
  Tests running...
  Ran 2 tests in 0.000s

  OK

For more information about the capabilities of the `TestCommand` command see::

	$ python setup.py test --help

You can use the `setup configuration`_ to specify the default behavior of the
`TestCommand` command.

Assertions
==========

The core of automated testing is making assertions about the way things are,
and getting a nice, helpful, informative error message when things are not as
they ought to be.

All of the assertions that you can find in Python standard unittest_ can be
found in testtools (remember, testtools extends unittest).  testtools changes
the behaviour of some of those assertions slightly and adds some new
assertions that you will almost certainly find useful.


Improved assertRaises
---------------------

``TestCase.assertRaises`` returns the caught exception.  This is useful for
asserting more things about the exception than just the type::

  def test_square_bad_input(self):
      # 'square' raises a TypeError if it's given bad input, say a
      # string.
      e = self.assertRaises(TypeError, silly.square, "orange")
      self.assertEqual("orange", e.bad_value)
      self.assertEqual("Cannot square 'orange', not a number.", str(e))

Note that this is incompatible with the ``assertRaises`` in unittest2 and
Python2.7.


ExpectedException
-----------------

If you are using a version of Python that supports the ``with`` context
manager syntax, you might prefer to use that syntax to ensure that code raises
particular errors.  ``ExpectedException`` does just that.  For example::

  def test_square_root_bad_input_2(self):
      # 'square' raises a TypeError if it's given bad input.
      with ExpectedException(TypeError, "Cannot square.*"):
          silly.square('orange')

The first argument to ``ExpectedException`` is the type of exception you
expect to see raised.  The second argument is optional, and can be either a
regular expression or a matcher. If it is a regular expression, the ``str()``
of the raised exception must match the regular expression. If it is a matcher,
then the raised exception object must match it. The optional third argument
``msg`` will cause the raised error to be annotated with that message.


assertIn, assertNotIn
---------------------

These two assertions check whether a value is in a sequence and whether a
value is not in a sequence.  They are "assert" versions of the ``in`` and
``not in`` operators.  For example::

  def test_assert_in_example(self):
      self.assertIn('a', 'cat')
      self.assertNotIn('o', 'cat')
      self.assertIn(5, list_of_primes_under_ten)
      self.assertNotIn(12, list_of_primes_under_ten)


assertIs, assertIsNot
---------------------

These two assertions check whether values are identical to one another.  This
is sometimes useful when you want to test something more strict than mere
equality.  For example::

  def test_assert_is_example(self):
      foo = [None]
      foo_alias = foo
      bar = [None]
      self.assertIs(foo, foo_alias)
      self.assertIsNot(foo, bar)
      self.assertEqual(foo, bar) # They are equal, but not identical


assertIsInstance
----------------

As much as we love duck-typing and polymorphism, sometimes you need to check
whether or not a value is of a given type.  This method does that.  For
example::

  def test_assert_is_instance_example(self):
      now = datetime.now()
      self.assertIsInstance(now, datetime)

Note that there is no ``assertIsNotInstance`` in testtools currently.


expectFailure
-------------

Sometimes it's useful to write tests that fail.  For example, you might want
to turn a bug report into a unit test, but you don't know how to fix the bug
yet.  Or perhaps you want to document a known, temporary deficiency in a
dependency.

testtools gives you the ``TestCase.expectFailure`` to help with this.  You use
it to say that you expect this assertion to fail.  When the test runs and the
assertion fails, testtools will report it as an "expected failure".

Here's an example::

  def test_expect_failure_example(self):
      self.expectFailure(
          "cats should be dogs", self.assertEqual, 'cats', 'dogs')

As long as 'cats' is not equal to 'dogs', the test will be reported as an
expected failure.

If ever by some miracle 'cats' becomes 'dogs', then testtools will report an
"unexpected success".  Unlike standard unittest, testtools treats this as
something that fails the test suite, like an error or a failure.


Matchers
========

The built-in assertion methods are very useful, they are the bread and butter
of writing tests.  However, soon enough you will probably want to write your
own assertions.  Perhaps there are domain specific things that you want to
check (e.g. assert that two widgets are aligned parallel to the flux grid), or
perhaps you want to check something that could almost but not quite be found
in some other standard library (e.g. assert that two paths point to the same
file).

When you are in such situations, you could either make a base class for your
project that inherits from ``testtools.TestCase`` and make sure that all of
your tests derive from that, *or* you could use the testtools ``Matcher``
system.


Using Matchers
--------------

Here's a really basic example using stock matchers found in testtools::

  import testtools
  from testtools.matchers import Equals

  class TestSquare(TestCase):
      def test_square(self):
         result = square(7)
         self.assertThat(result, Equals(49))

The line ``self.assertThat(result, Equals(49))`` is equivalent to
``self.assertEqual(result, 49)`` and means "assert that ``result`` equals 49".
The difference is that ``assertThat`` is a more general method that takes some
kind of observed value (in this case, ``result``) and any matcher object
(here, ``Equals(49)``).

The matcher object could be absolutely anything that implements the Matcher
protocol.  This means that you can make more complex matchers by combining
existing ones::

  def test_square_silly(self):
      result = square(7)
      self.assertThat(result, Not(Equals(50)))

Which is roughly equivalent to::

  def test_square_silly(self):
      result = square(7)
      self.assertNotEqual(result, 50)


Stock matchers
--------------

testtools comes with many matchers built in.  They can all be found in and
imported from the ``testtools.matchers`` module.

Equals
~~~~~~

Matches if two items are equal. For example::

  def test_equals_example(self):
      self.assertThat([42], Equals([42]))


Is
~~~

Matches if two items are identical.  For example::

  def test_is_example(self):
      foo = object()
      self.assertThat(foo, Is(foo))


IsInstance
~~~~~~~~~~

Adapts isinstance() to use as a matcher.  For example::

  def test_isinstance_example(self):
      class MyClass:pass
      self.assertThat(MyClass(), IsInstance(MyClass))
      self.assertThat(MyClass(), IsInstance(MyClass, str))


The raises helper
~~~~~~~~~~~~~~~~~

Matches if a callable raises a particular type of exception.  For example::

  def test_raises_example(self):
      self.assertThat(lambda: 1/0, raises(ZeroDivisionError))

This is actually a convenience function that combines two other matchers:
Raises_ and MatchesException_.


DocTestMatches
~~~~~~~~~~~~~~

Matches a string as if it were the output of a doctest_ example.  Very useful
for making assertions about large chunks of text.  For example::

  import doctest

  def test_doctest_example(self):
      output = "Colorless green ideas"
      self.assertThat(
          output,
          DocTestMatches("Colorless ... ideas", doctest.ELLIPSIS))

We highly recommend using the following flags::

  doctest.ELLIPSIS | doctest.NORMALIZE_WHITESPACE | doctest.REPORT_NDIFF


GreaterThan
~~~~~~~~~~~

Matches if the given thing is greater than the thing in the matcher.  For
example::

  def test_greater_than_example(self):
      self.assertThat(3, GreaterThan(2))


LessThan
~~~~~~~~

Matches if the given thing is less than the thing in the matcher.  For
example::

  def test_less_than_example(self):
      self.assertThat(2, LessThan(3))


StartsWith, EndsWith
~~~~~~~~~~~~~~~~~~~~

These matchers check to see if a string starts with or ends with a particular
substring.  For example::

  def test_starts_and_ends_with_example(self):
      self.assertThat('underground', StartsWith('und'))
      self.assertThat('underground', EndsWith('und'))


Contains
~~~~~~~~

This matcher checks to see if the given thing contains the thing in the
matcher.  For example::

  def test_contains_example(self):
      self.assertThat('abc', Contains('b'))


MatchesException
~~~~~~~~~~~~~~~~

Matches an exc_info tuple if the exception is of the correct type.  For
example::

  def test_matches_exception_example(self):
      try:
          raise RuntimeError('foo')
      except RuntimeError:
          exc_info = sys.exc_info()
      self.assertThat(exc_info, MatchesException(RuntimeError))
      self.assertThat(exc_info, MatchesException(RuntimeError('bar'))

Most of the time, you will want to uses `The raises helper`_ instead.


NotEquals
~~~~~~~~~

Matches if something is not equal to something else.  Note that this is subtly
different to ``Not(Equals(x))``.  ``NotEquals(x)`` will match if ``y != x``,
``Not(Equals(x))`` will match if ``not y == x``.

You only need to worry about this distinction if you are testing code that
relies on badly written overloaded equality operators.


KeysEqual
~~~~~~~~~

Matches if the keys of one dict are equal to the keys of another dict.  For
example::

  def test_keys_equal(self):
      x = {'a': 1, 'b': 2}
      y = {'a': 2, 'b': 3}
      self.assertThat(x, KeysEqual(y))


MatchesRegex
~~~~~~~~~~~~

Matches a string against a regular expression, which is a wonderful thing to
be able to do, if you think about it::

  def test_matches_regex_example(self):
      self.assertThat('foo', MatchesRegex('fo+'))


HasLength
~~~~~~~~~

Check the length of a collection.  The following assertion will fail::

  self.assertThat([1, 2, 3], HasLength(2))

But this one won't::

  self.assertThat([1, 2, 3], HasLength(3))


File- and path-related matchers
-------------------------------

testtools also has a number of matchers to help with asserting things about
the state of the filesystem.

PathExists
~~~~~~~~~~

Matches if a path exists::

  self.assertThat('/', PathExists())


DirExists
~~~~~~~~~

Matches if a path exists and it refers to a directory::

  # This will pass on most Linux systems.
  self.assertThat('/home/', DirExists())
  # This will not
  self.assertThat('/home/jml/some-file.txt', DirExists())


FileExists
~~~~~~~~~~

Matches if a path exists and it refers to a file (as opposed to a directory)::

  # This will pass on most Linux systems.
  self.assertThat('/bin/true', FileExists())
  # This will not.
  self.assertThat('/home/', FileExists())


DirContains
~~~~~~~~~~~

Matches if the given directory contains the specified files and directories.
Say we have a directory ``foo`` that has the files ``a``, ``b`` and ``c``,
then::

  self.assertThat('foo', DirContains(['a', 'b', 'c']))

will match, but::

  self.assertThat('foo', DirContains(['a', 'b']))

will not.

The matcher sorts both the input and the list of names we get back from the
filesystem.

You can use this in a more advanced way, and match the sorted directory
listing against an arbitrary matcher::

  self.assertThat('foo', DirContains(matcher=Contains('a')))


FileContains
~~~~~~~~~~~~

Matches if the given file has the specified contents.  Say there's a file
called ``greetings.txt`` with the contents, ``Hello World!``::

  self.assertThat('greetings.txt', FileContains("Hello World!"))

will match.

You can also use this in a more advanced way, and match the contents of the
file against an arbitrary matcher::

  self.assertThat('greetings.txt', FileContains(matcher=Contains('!')))


HasPermissions
~~~~~~~~~~~~~~

Used for asserting that a file or directory has certain permissions.  Uses
octal-mode permissions for both input and matching.  For example::

  self.assertThat('/tmp', HasPermissions('1777'))
  self.assertThat('id_rsa', HasPermissions('0600'))

This is probably more useful on UNIX systems than on Windows systems.


SamePath
~~~~~~~~

Matches if two paths actually refer to the same thing.  The paths don't have
to exist, but if they do exist, ``SamePath`` will resolve any symlinks.::

  self.assertThat('somefile', SamePath('childdir/../somefile'))


TarballContains
~~~~~~~~~~~~~~~

Matches the contents of a tarball.  In many ways, much like ``DirContains``,
but instead of matching on ``os.listdir`` matches on ``TarFile.getnames``.


Combining matchers
------------------

One great thing about matchers is that you can readily combine existing
matchers to get variations on their behaviour or to quickly build more complex
assertions.

Below are a few of the combining matchers that come with testtools.


Not
~~~

Negates another matcher.  For example::

  def test_not_example(self):
      self.assertThat([42], Not(Equals("potato")))
      self.assertThat([42], Not(Is([42])))

If you find yourself using ``Not`` frequently, you may wish to create a custom
matcher for it.  For example::

  IsNot = lambda x: Not(Is(x))

  def test_not_example_2(self):
      self.assertThat([42], IsNot([42]))


Annotate
~~~~~~~~

Used to add custom notes to a matcher.  For example::

  def test_annotate_example(self):
      result = 43
      self.assertThat(
          result, Annotate("Not the answer to the Question!", Equals(42))

Since the annotation is only ever displayed when there is a mismatch
(e.g. when ``result`` does not equal 42), it's a good idea to phrase the note
negatively, so that it describes what a mismatch actually means.

As with Not_, you may wish to create a custom matcher that describes a
common operation.  For example::

  PoliticallyEquals = lambda x: Annotate("Death to the aristos!", Equals(x))

  def test_annotate_example_2(self):
      self.assertThat("orange", PoliticallyEquals("yellow"))

You can have assertThat perform the annotation for you as a convenience::

  def test_annotate_example_3(self):
      self.assertThat("orange", Equals("yellow"), "Death to the aristos!")


AfterPreprocessing
~~~~~~~~~~~~~~~~~~

Used to make a matcher that applies a function to the matched object before
matching. This can be used to aid in creating trivial matchers as functions, for
example::

  def test_after_preprocessing_example(self):
      def PathHasFileContent(content):
          def _read(path):
              return open(path).read()
          return AfterPreprocessing(_read, Equals(content))
      self.assertThat('/tmp/foo.txt', PathHasFileContent("Hello world!"))


MatchesAll
~~~~~~~~~~

Combines many matchers to make a new matcher.  The new matcher will only match
things that match every single one of the component matchers.

It's much easier to understand in Python than in English::

  def test_matches_all_example(self):
      has_und_at_both_ends = MatchesAll(StartsWith("und"), EndsWith("und"))
      # This will succeed.
      self.assertThat("underground", has_und_at_both_ends)
      # This will fail.
      self.assertThat("found", has_und_at_both_ends)
      # So will this.
      self.assertThat("undead", has_und_at_both_ends)

At this point some people ask themselves, "why bother doing this at all? why
not just have two separate assertions?".  It's a good question.

The first reason is that when a ``MatchesAll`` gets a mismatch, the error will
include information about all of the bits that mismatched.  When you have two
separate assertions, as below::

  def test_two_separate_assertions(self):
       self.assertThat("foo", StartsWith("und"))
       self.assertThat("foo", EndsWith("und"))

Then you get absolutely no information from the second assertion if the first
assertion fails.  Tests are largely there to help you debug code, so having
more information in error messages is a big help.

The second reason is that it is sometimes useful to give a name to a set of
matchers. ``has_und_at_both_ends`` is a bit contrived, of course, but it is
clear.  The ``FileExists`` and ``DirExists`` matchers included in testtools
are perhaps better real examples.

If you want only the first mismatch to be reported, pass ``first_only=True``
as a keyword parameter to ``MatchesAll``.


MatchesAny
~~~~~~~~~~

Like MatchesAll_, ``MatchesAny`` combines many matchers to make a new
matcher.  The difference is that the new matchers will match a thing if it
matches *any* of the component matchers.

For example::

  def test_matches_any_example(self):
      self.assertThat(42, MatchesAny(Equals(5), Not(Equals(6))))


AllMatch
~~~~~~~~

Matches many values against a single matcher.  Can be used to make sure that
many things all meet the same condition::

  def test_all_match_example(self):
      self.assertThat([2, 3, 5, 7], AllMatch(LessThan(10)))

If the match fails, then all of the values that fail to match will be included
in the error message.

In some ways, this is the converse of MatchesAll_.


MatchesListwise
~~~~~~~~~~~~~~~

Where ``MatchesAny`` and ``MatchesAll`` combine many matchers to match a
single value, ``MatchesListwise`` combines many matches to match many values.

For example::

  def test_matches_listwise_example(self):
      self.assertThat(
          [1, 2, 3], MatchesListwise(map(Equals, [1, 2, 3])))

This is useful for writing custom, domain-specific matchers.

If you want only the first mismatch to be reported, pass ``first_only=True``
to ``MatchesListwise``.


MatchesSetwise
~~~~~~~~~~~~~~

Combines many matchers to match many values, without regard to their order.

Here's an example::

  def test_matches_setwise_example(self):
      self.assertThat(
          [1, 2, 3], MatchesSetwise(Equals(2), Equals(3), Equals(1)))

Much like ``MatchesListwise``, best used for writing custom, domain-specific
matchers.


MatchesStructure
~~~~~~~~~~~~~~~~

Creates a matcher that matches certain attributes of an object against a
pre-defined set of matchers.

It's much easier to understand in Python than in English::

  def test_matches_structure_example(self):
      foo = Foo()
      foo.a = 1
      foo.b = 2
      matcher = MatchesStructure(a=Equals(1), b=Equals(2))
      self.assertThat(foo, matcher)

Since all of the matchers used were ``Equals``, we could also write this using
the ``byEquality`` helper::

  def test_matches_structure_example(self):
      foo = Foo()
      foo.a = 1
      foo.b = 2
      matcher = MatchesStructure.byEquality(a=1, b=2)
      self.assertThat(foo, matcher)

``MatchesStructure.fromExample`` takes an object and a list of attributes and
creates a ``MatchesStructure`` matcher where each attribute of the matched
object must equal each attribute of the example object.  For example::

      matcher = MatchesStructure.fromExample(foo, 'a', 'b')

is exactly equivalent to ``matcher`` in the previous example.


MatchesPredicate
~~~~~~~~~~~~~~~~

Sometimes, all you want to do is create a matcher that matches if a given
function returns True, and mismatches if it returns False.

For example, you might have an ``is_prime`` function and want to make a
matcher based on it::

  def test_prime_numbers(self):
      IsPrime = MatchesPredicate(is_prime, '%s is not prime.')
      self.assertThat(7, IsPrime)
      self.assertThat(1983, IsPrime)
      # This will fail.
      self.assertThat(42, IsPrime)

Which will produce the error message::

  Traceback (most recent call last):
    File "...", line ..., in test_prime_numbers
      self.assertThat(42, IsPrime)
  MismatchError: 42 is not prime.


MatchesPredicateWithParams
~~~~~~~~~~~~~~~~~~~~~~~~~~

Sometimes you can't use a trivial predicate and instead need to pass in some
parameters each time. In that case, MatchesPredicateWithParams is your go-to
tool for creating ad hoc matchers. MatchesPredicateWithParams takes a predicate
function and message and returns a factory to produce matchers from that. The
predicate needs to return a boolean (or any truthy object), and accept the
object to match + whatever was passed into the factory.

For example, you might have an ``divisible`` function and want to make a
matcher based on it::

  def test_divisible_numbers(self):
      IsDivisibleBy = MatchesPredicateWithParams(
          divisible, '{0} is not divisible by {1}')
      self.assertThat(7, IsDivisibleBy(1))
      self.assertThat(7, IsDivisibleBy(7))
      self.assertThat(7, IsDivisibleBy(2)))
      # This will fail.

Which will produce the error message::

  Traceback (most recent call last):
    File "...", line ..., in test_divisible
      self.assertThat(7, IsDivisibleBy(2))
  MismatchError: 7 is not divisible by 2.


Raises
~~~~~~

Takes whatever the callable raises as an exc_info tuple and matches it against
whatever matcher it was given.  For example, if you want to assert that a
callable raises an exception of a given type::

  def test_raises_example(self):
      self.assertThat(
          lambda: 1/0, Raises(MatchesException(ZeroDivisionError)))

Although note that this could also be written as::

  def test_raises_example_convenient(self):
      self.assertThat(lambda: 1/0, raises(ZeroDivisionError))

See also MatchesException_ and `the raises helper`_


Writing your own matchers
-------------------------

Combining matchers is fun and can get you a very long way indeed, but
sometimes you will have to write your own.  Here's how.

You need to make two closely-linked objects: a ``Matcher`` and a
``Mismatch``.  The ``Matcher`` knows how to actually make the comparison, and
the ``Mismatch`` knows how to describe a failure to match.

Here's an example matcher::

  class IsDivisibleBy(object):
      """Match if a number is divisible by another number."""
      def __init__(self, divider):
          self.divider = divider
      def __str__(self):
          return 'IsDivisibleBy(%s)' % (self.divider,)
      def match(self, actual):
          remainder = actual % self.divider
          if remainder != 0:
              return IsDivisibleByMismatch(actual, self.divider, remainder)
          else:
              return None

The matcher has a constructor that takes parameters that describe what you
actually *expect*, in this case a number that other numbers ought to be
divisible by.  It has a ``__str__`` method, the result of which is displayed
on failure by ``assertThat`` and a ``match`` method that does the actual
matching.

``match`` takes something to match against, here ``actual``, and decides
whether or not it matches.  If it does match, then ``match`` must return
``None``.  If it does *not* match, then ``match`` must return a ``Mismatch``
object. ``assertThat`` will call ``match`` and then fail the test if it
returns a non-None value.  For example::

  def test_is_divisible_by_example(self):
      # This succeeds, since IsDivisibleBy(5).match(10) returns None.
      self.assertThat(10, IsDivisibleBy(5))
      # This fails, since IsDivisibleBy(7).match(10) returns a mismatch.
      self.assertThat(10, IsDivisibleBy(7))

The mismatch is responsible for what sort of error message the failing test
generates.  Here's an example mismatch::

  class IsDivisibleByMismatch(object):
      def __init__(self, number, divider, remainder):
          self.number = number
          self.divider = divider
          self.remainder = remainder

      def describe(self):
          return "%r is not divisible by %r, %r remains" % (
              self.number, self.divider, self.remainder)

      def get_details(self):
          return {}

The mismatch takes information about the mismatch, and provides a ``describe``
method that assembles all of that into a nice error message for end users.
You can use the ``get_details`` method to provide extra, arbitrary data with
the mismatch (e.g. the contents of a log file).  Most of the time it's fine to
just return an empty dict.  You can read more about Details_ elsewhere in this
document.

Sometimes you don't need to create a custom mismatch class.  In particular, if
you don't care *when* the description is calculated, then you can just do that
in the Matcher itself like this::

  def match(self, actual):
      remainder = actual % self.divider
      if remainder != 0:
          return Mismatch(
              "%r is not divisible by %r, %r remains" % (
                  actual, self.divider, remainder))
      else:
          return None

When writing a ``describe`` method or constructing a ``Mismatch`` object the
code should ensure it only emits printable unicode.  As this output must be
combined with other text and forwarded for presentation, letting through
non-ascii bytes of ambiguous encoding or control characters could throw an
exception or mangle the display.  In most cases simply avoiding the ``%s``
format specifier and using ``%r`` instead will be enough.  For examples of
more complex formatting see the ``testtools.matchers`` implementatons.


Details
=======

As we may have mentioned once or twice already, one of the great benefits of
automated tests is that they help find, isolate and debug errors in your
system.

Frequently however, the information provided by a mere assertion failure is
not enough.  It's often useful to have other information: the contents of log
files; what queries were run; benchmark timing information; what state certain
subsystem components are in and so forth.

testtools calls all of these things "details" and provides a single, powerful
mechanism for including this information in your test run.

Here's an example of how to add them::

  from testtools import TestCase
  from testtools.content import text_content

  class TestSomething(TestCase):

      def test_thingy(self):
          self.addDetail('arbitrary-color-name', text_content("blue"))
          1 / 0 # Gratuitous error!

A detail an arbitrary piece of content given a name that's unique within the
test.  Here the name is ``arbitrary-color-name`` and the content is
``text_content("blue")``.  The name can be any text string, and the content
can be any ``testtools.content.Content`` object.

When the test runs, testtools will show you something like this::

  ======================================================================
  ERROR: exampletest.TestSomething.test_thingy
  ----------------------------------------------------------------------
  arbitrary-color-name: {{{blue}}}

  Traceback (most recent call last):
    File "exampletest.py", line 8, in test_thingy
      1 / 0 # Gratuitous error!
  ZeroDivisionError: integer division or modulo by zero
  ------------
  Ran 1 test in 0.030s

As you can see, the detail is included as an attachment, here saying
that our arbitrary-color-name is "blue".


Content
-------

For the actual content of details, testtools uses its own MIME-based Content
object.  This allows you to attach any information that you could possibly
conceive of to a test, and allows testtools to use or serialize that
information.

The basic ``testtools.content.Content`` object is constructed from a
``testtools.content.ContentType`` and a nullary callable that must return an
iterator of chunks of bytes that the content is made from.

So, to make a Content object that is just a simple string of text, you can
do::

  from testtools.content import Content
  from testtools.content_type import ContentType

  text = Content(ContentType('text', 'plain'), lambda: ["some text"])

Because adding small bits of text content is very common, there's also a
convenience method::

  text = text_content("some text")

To make content out of an image stored on disk, you could do something like::

  image = Content(ContentType('image', 'png'), lambda: open('foo.png').read())

Or you could use the convenience function::

  image = content_from_file('foo.png', ContentType('image', 'png'))

The ``lambda`` helps make sure that the file is opened and the actual bytes
read only when they are needed – by default, when the test is finished.  This
means that tests can construct and add Content objects freely without worrying
too much about how they affect run time.


A realistic example
-------------------

A very common use of details is to add a log file to failing tests.  Say your
project has a server represented by a class ``SomeServer`` that you can start
up and shut down in tests, but runs in another process.  You want to test
interaction with that server, and whenever the interaction fails, you want to
see the client-side error *and* the logs from the server-side.  Here's how you
might do it::

  from testtools import TestCase
  from testtools.content import attach_file, Content
  from testtools.content_type import UTF8_TEXT

  from myproject import SomeServer

  class SomeTestCase(TestCase):

      def setUp(self):
          super(SomeTestCase, self).setUp()
          self.server = SomeServer()
          self.server.start_up()
          self.addCleanup(self.server.shut_down)
          self.addCleanup(attach_file, self.server.logfile, self)

      def attach_log_file(self):
          self.addDetail(
              'log-file',
              Content(UTF8_TEXT,
                      lambda: open(self.server.logfile, 'r').readlines()))

      def test_a_thing(self):
          self.assertEqual("cool", self.server.temperature)

This test will attach the log file of ``SomeServer`` to each test that is
run.  testtools will only display the log file for failing tests, so it's not
such a big deal.

If the act of adding at detail is expensive, you might want to use
addOnException_ so that you only do it when a test actually raises an
exception.


Controlling test execution
==========================

.. _addCleanup:

addCleanup
----------

``TestCase.addCleanup`` is a robust way to arrange for a clean up function to
be called before ``tearDown``.  This is a powerful and simple alternative to
putting clean up logic in a try/finally block or ``tearDown`` method.  For
example::

  def test_foo(self):
      foo.lock()
      self.addCleanup(foo.unlock)
      ...

This is particularly useful if you have some sort of factory in your test::

  def make_locked_foo(self):
      foo = Foo()
      foo.lock()
      self.addCleanup(foo.unlock)
      return foo

  def test_frotz_a_foo(self):
      foo = self.make_locked_foo()
      foo.frotz()
      self.assertEqual(foo.frotz_count, 1)

Any extra arguments or keyword arguments passed to ``addCleanup`` are passed
to the callable at cleanup time.

Cleanups can also report multiple errors, if appropriate by wrapping them in
a ``testtools.MultipleExceptions`` object::

  raise MultipleExceptions(exc_info1, exc_info2)


Fixtures
--------

Tests often depend on a system being set up in a certain way, or having
certain resources available to them.  Perhaps a test needs a connection to the
database or access to a running external server.

One common way of doing this is to do::

  class SomeTest(TestCase):
      def setUp(self):
          super(SomeTest, self).setUp()
          self.server = Server()
          self.server.setUp()
          self.addCleanup(self.server.tearDown)

testtools provides a more convenient, declarative way to do the same thing::

  class SomeTest(TestCase):
      def setUp(self):
          super(SomeTest, self).setUp()
          self.server = self.useFixture(Server())

``useFixture(fixture)`` calls ``setUp`` on the fixture, schedules a clean up
to clean it up, and schedules a clean up to attach all details_ held by the
fixture to the test case.  The fixture object must meet the
``fixtures.Fixture`` protocol (version 0.3.4 or newer, see fixtures_).

If you have anything beyond the most simple test set up, we recommend that
you put this set up into a ``Fixture`` class.  Once there, the fixture can be
easily re-used by other tests and can be combined with other fixtures to make
more complex resources.


Skipping tests
--------------

Many reasons exist to skip a test: a dependency might be missing; a test might
be too expensive and thus should not berun while on battery power; or perhaps
the test is testing an incomplete feature.

``TestCase.skipTest`` is a simple way to have a test stop running and be
reported as a skipped test, rather than a success, error or failure.  For
example::

  def test_make_symlink(self):
      symlink = getattr(os, 'symlink', None)
      if symlink is None:
          self.skipTest("No symlink support")
      symlink(whatever, something_else)

Using ``skipTest`` means that you can make decisions about what tests to run
as late as possible, and close to the actual tests.  Without it, you might be
forced to use convoluted logic during test loading, which is a bit of a mess.


Legacy skip support
~~~~~~~~~~~~~~~~~~~

If you are using this feature when running your test suite with a legacy
``TestResult`` object that is missing the ``addSkip`` method, then the
``addError`` method will be invoked instead.  If you are using a test result
from testtools, you do not have to worry about this.

In older versions of testtools, ``skipTest`` was known as ``skip``. Since
Python 2.7 added ``skipTest`` support, the ``skip`` name is now deprecated.
No warning is emitted yet – some time in the future we may do so.


addOnException
--------------

Sometimes, you might wish to do something only when a test fails.  Perhaps you
need to run expensive diagnostic routines or some such.
``TestCase.addOnException`` allows you to easily do just this.  For example::

  class SomeTest(TestCase):
      def setUp(self):
          super(SomeTest, self).setUp()
          self.server = self.useFixture(SomeServer())
          self.addOnException(self.attach_server_diagnostics)

      def attach_server_diagnostics(self, exc_info):
          self.server.prep_for_diagnostics() # Expensive!
          self.addDetail('server-diagnostics', self.server.get_diagnostics)

      def test_a_thing(self):
          self.assertEqual('cheese', 'chalk')

In this example, ``attach_server_diagnostics`` will only be called when a test
fails.  It is given the exc_info tuple of the error raised by the test, just
in case it is needed.


Twisted support
---------------

testtools provides *highly experimental* support for running Twisted tests –
tests that return a Deferred_ and rely on the Twisted reactor.  You should not
use this feature right now.  We reserve the right to change the API and
behaviour without telling you first.

However, if you are going to, here's how you do it::

  from testtools import TestCase
  from testtools.deferredruntest import AsynchronousDeferredRunTest

  class MyTwistedTests(TestCase):

      run_tests_with = AsynchronousDeferredRunTest

      def test_foo(self):
          # ...
          return d

In particular, note that you do *not* have to use a special base ``TestCase``
in order to run Twisted tests.

You can also run individual tests within a test case class using the Twisted
test runner::

   class MyTestsSomeOfWhichAreTwisted(TestCase):

       def test_normal(self):
           pass

       @run_test_with(AsynchronousDeferredRunTest)
       def test_twisted(self):
           # ...
           return d

Here are some tips for converting your Trial tests into testtools tests.

* Use the ``AsynchronousDeferredRunTest`` runner
* Make sure to upcall to ``setUp`` and ``tearDown``
* Don't use ``setUpClass`` or ``tearDownClass``
* Don't expect setting .todo, .timeout or .skip attributes to do anything
* ``flushLoggedErrors`` is ``testtools.deferredruntest.flush_logged_errors``
* ``assertFailure`` is ``testtools.deferredruntest.assert_fails_with``
* Trial spins the reactor a couple of times before cleaning it up,
  ``AsynchronousDeferredRunTest`` does not.  If you rely on this behavior, use
  ``AsynchronousDeferredRunTestForBrokenTwisted``.

force_failure
-------------

Setting the ``testtools.TestCase.force_failure`` instance variable to ``True``
will cause the test to be marked as a failure, but won't stop the test code
from running (see :ref:`force_failure`).


Test helpers
============

testtools comes with a few little things that make it a little bit easier to
write tests.


TestCase.patch
--------------

``patch`` is a convenient way to monkey-patch a Python object for the duration
of your test.  It's especially useful for testing legacy code.  e.g.::

  def test_foo(self):
      my_stream = StringIO()
      self.patch(sys, 'stderr', my_stream)
      run_some_code_that_prints_to_stderr()
      self.assertEqual('', my_stream.getvalue())

The call to ``patch`` above masks ``sys.stderr`` with ``my_stream`` so that
anything printed to stderr will be captured in a StringIO variable that can be
actually tested. Once the test is done, the real ``sys.stderr`` is restored to
its rightful place.


Creation methods
----------------

Often when writing unit tests, you want to create an object that is a
completely normal instance of its type.  You don't want there to be anything
special about its properties, because you are testing generic behaviour rather
than specific conditions.

A lot of the time, test authors do this by making up silly strings and numbers
and passing them to constructors (e.g. 42, 'foo', "bar" etc), and that's
fine.  However, sometimes it's useful to be able to create arbitrary objects
at will, without having to make up silly sample data.

To help with this, ``testtools.TestCase`` implements creation methods called
``getUniqueString`` and ``getUniqueInteger``.  They return strings and
integers that are unique within the context of the test that can be used to
assemble more complex objects.  Here's a basic example where
``getUniqueString`` is used instead of saying "foo" or "bar" or whatever::

  class SomeTest(TestCase):

      def test_full_name(self):
          first_name = self.getUniqueString()
          last_name = self.getUniqueString()
          p = Person(first_name, last_name)
          self.assertEqual(p.full_name, "%s %s" % (first_name, last_name))


And here's how it could be used to make a complicated test::

  class TestCoupleLogic(TestCase):

      def make_arbitrary_person(self):
          return Person(self.getUniqueString(), self.getUniqueString())

      def test_get_invitation(self):
          a = self.make_arbitrary_person()
          b = self.make_arbitrary_person()
          couple = Couple(a, b)
          event_name = self.getUniqueString()
          invitation = couple.get_invitation(event_name)
          self.assertEqual(
              invitation,
              "We invite %s and %s to %s" % (
                  a.full_name, b.full_name, event_name))

Essentially, creation methods like these are a way of reducing the number of
assumptions in your tests and communicating to test readers that the exact
details of certain variables don't actually matter.

See pages 419-423 of `xUnit Test Patterns`_ by Gerard Meszaros for a detailed
discussion of creation methods.

Test attributes
---------------

Inspired by the ``nosetests`` ``attr`` plugin, testtools provides support for
marking up test methods with attributes, which are then exposed in the test
id and can be used when filtering tests by id. (e.g. via ``--load-list``)::

  from testtools.testcase import attr, WithAttributes
  
  class AnnotatedTests(WithAttributes, TestCase):

      @attr('simple')
      def test_one(self):
          pass
      
      @attr('more', 'than', 'one)
      def test_two(self):
          pass

      @attr('or')
      @attr('stacked')
      def test_three(self):
          pass

General helpers
===============

Conditional imports
-------------------

Lots of the time we would like to conditionally import modules.  testtools
uses the small library extras to do this. This used to be part of testtools.

Instead of::

  try:
      from twisted.internet import defer
  except ImportError:
      defer = None

You can do::

   defer = try_import('twisted.internet.defer')


Instead of::

  try:
      from StringIO import StringIO
  except ImportError:
      from io import StringIO

You can do::

  StringIO = try_imports(['StringIO.StringIO', 'io.StringIO'])


Safe attribute testing
----------------------

``hasattr`` is broken_ on many versions of Python. The helper ``safe_hasattr``
can be used to safely test whether an object has a particular attribute. Like
``try_import`` this used to be in testtools but is now in extras.


Nullary callables
-----------------

Sometimes you want to be able to pass around a function with the arguments
already specified.  The normal way of doing this in Python is::

  nullary = lambda: f(*args, **kwargs)
  nullary()

Which is mostly good enough, but loses a bit of debugging information.  If you
take the ``repr()`` of ``nullary``, you're only told that it's a lambda, and
you get none of the juicy meaning that you'd get from the ``repr()`` of ``f``.

The solution is to use ``Nullary`` instead::

  nullary = Nullary(f, *args, **kwargs)
  nullary()

Here, ``repr(nullary)`` will be the same as ``repr(f)``.


.. _testrepository: https://launchpad.net/testrepository
.. _Trial: http://twistedmatrix.com/documents/current/core/howto/testing.html
.. _nose: http://somethingaboutorange.com/mrl/projects/nose/
.. _unittest2: http://pypi.python.org/pypi/unittest2
.. _zope.testrunner: http://pypi.python.org/pypi/zope.testrunner/
.. _xUnit test patterns: http://xunitpatterns.com/
.. _fixtures: http://pypi.python.org/pypi/fixtures
.. _unittest: http://docs.python.org/library/unittest.html
.. _doctest: http://docs.python.org/library/doctest.html
.. _Deferred: http://twistedmatrix.com/documents/current/core/howto/defer.html
.. _discover: http://pypi.python.org/pypi/discover
.. _`testtools API docs`: http://mumak.net/testtools/apidocs/
.. _Distutils: http://docs.python.org/library/distutils.html
.. _`setup configuration`: http://docs.python.org/distutils/configfile.html
.. _broken: http://chipaca.com/post/3210673069/hasattr-17-less-harmful
