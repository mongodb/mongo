=========================
Contributing to testtools
=========================

Coding style
------------

In general, follow `PEP 8`_ except where consistency with the standard
library's unittest_ module would suggest otherwise.

testtools currently supports Python 2.6 and later, including Python 3.

Copyright assignment
--------------------

Part of testtools raison d'etre is to provide Python with improvements to the
testing code it ships. For that reason we require all contributions (that are
non-trivial) to meet one of the following rules:

* be inapplicable for inclusion in Python.
* be able to be included in Python without further contact with the contributor.
* be copyright assigned to Jonathan M. Lange.

Please pick one of these and specify it when contributing code to testtools.


Licensing
---------

All code that is not copyright assigned to Jonathan M. Lange (see Copyright
Assignment above) needs to be licensed under the `MIT license`_ that testtools
uses, so that testtools can ship it.


Testing
-------

Please write tests for every feature.  This project ought to be a model
example of well-tested Python code!

Take particular care to make sure the *intent* of each test is clear.

You can run tests with ``make check``.

By default, testtools hides many levels of its own stack when running tests.
This is for the convenience of users, who do not care about how, say, assert
methods are implemented. However, when writing tests for testtools itself, it
is often useful to see all levels of the stack. To do this, add
``run_tests_with = FullStackRunTest`` to the top of a test's class definition.


Discussion
----------

When submitting a patch, it will help the review process a lot if there's a
clear explanation of what the change does and why you think the change is a
good idea.  For crasher bugs, this is generally a no-brainer, but for UI bugs
& API tweaks, the reason something is an improvement might not be obvious, so
it's worth spelling out.

If you are thinking of implementing a new feature, you might want to have that
discussion on the [mailing list](testtools-dev@lists.launchpad.net) before the
patch goes up for review.  This is not at all mandatory, but getting feedback
early can help avoid dead ends.


Documentation
-------------

Documents are written using the Sphinx_ variant of reStructuredText_.  All
public methods, functions, classes and modules must have API documentation.
When changing code, be sure to check the API documentation to see if it could
be improved.  Before submitting changes to trunk, look over them and see if
the manuals ought to be updated.


Source layout
-------------

The top-level directory contains the ``testtools/`` package directory, and
miscellaneous files like ``README.rst`` and ``setup.py``.

The ``testtools/`` directory is the Python package itself.  It is separated
into submodules for internal clarity, but all public APIs should be “promoted”
into the top-level package by importing them in ``testtools/__init__.py``.
Users of testtools should never import a submodule in order to use a stable
API.  Unstable APIs like ``testtools.matchers`` and
``testtools.deferredruntest`` should be exported as submodules.

Tests belong in ``testtools/tests/``.


Committing to trunk
-------------------

Testtools is maintained using git, with its master repo at https://github.com
/testing-cabal/testtools. This gives every contributor the ability to commit
their work to their own branches. However permission must be granted to allow
contributors to commit to the trunk branch.

Commit access to trunk is obtained by joining the `testing-cabal`_, either as an
Owner or a Committer. Commit access is contingent on obeying the testtools
contribution policy, see `Copyright Assignment`_ above.


Code Review
-----------

All code must be reviewed before landing on trunk. The process is to create a
branch on Github, and make a pull request into trunk. It will then be reviewed
before it can be merged to trunk. It will be reviewed by someone:

* not the author
* a committer

As a special exception, since there are few testtools committers and thus
reviews are prone to blocking, a pull request from a committer that has not been
reviewed after 24 hours may be merged by that committer. When the team is larger
this policy will be revisited.

Code reviewers should look for the quality of what is being submitted,
including conformance with this HACKING file.

Changes which all users should be made aware of should be documented in NEWS.


NEWS management
---------------

The file NEWS is structured as a sorted list of releases. Each release can have
a free form description and more or more sections with bullet point items.
Sections in use today are 'Improvements' and 'Changes'. To ease merging between
branches, the bullet points are kept alphabetically sorted. The release NEXT is
permanently present at the top of the list.


Release tasks
-------------

#. Choose a version number, say X.Y.Z
#. In trunk, ensure __init__ has version ``(X, Y, Z, 'final', 0)``
#. Under NEXT in NEWS add a heading with the version number X.Y.Z.
#. Possibly write a blurb into NEWS.
#. Commit the changes.
#. Tag the release, ``git tag -s testtools-X.Y.Z``
#. Run 'make release', this:
   #. Creates a source distribution and uploads to PyPI
   #. Ensures all Fix Committed bugs are in the release milestone
   #. Makes a release on Launchpad and uploads the tarball
   #. Marks all the Fix Committed bugs as Fix Released
   #. Creates a new milestone
#. Change __version__ in __init__.py to the probable next version.
   e.g. to ``(X, Y, Z+1, 'dev', 0)``.
#. Commit 'Opening X.Y.Z+1 for development.'
#. If a new series has been created (e.g. 0.10.0), make the series on Launchpad.
#. Push trunk to Github, ``git push --tags origin master``

.. _PEP 8: http://www.python.org/dev/peps/pep-0008/
.. _unittest: http://docs.python.org/library/unittest.html
.. _MIT license: http://www.opensource.org/licenses/mit-license.php
.. _Sphinx: http://sphinx.pocoo.org/
.. _restructuredtext: http://docutils.sourceforge.net/rst.html
.. _testing-cabal: https://github.com/organizations/testing-cabal/
