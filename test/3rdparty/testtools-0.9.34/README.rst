=========
testtools
=========

testtools is a set of extensions to the Python standard library's unit testing
framework.

These extensions have been derived from years of experience with unit testing
in Python and come from many different sources.


Documentation
-------------

If you would like to learn more about testtools, consult our documentation in
the 'doc/' directory.  You might like to start at 'doc/overview.rst' or
'doc/for-test-authors.rst'.


Licensing
---------

This project is distributed under the MIT license and copyright is owned by
Jonathan M. Lange and the testtools authors. See LICENSE for details.

Some code in 'testtools/run.py' is taken from Python's unittest module, and is
copyright Steve Purcell and the Python Software Foundation, it is distributed
under the same license as Python, see LICENSE for details.


Required Dependencies
---------------------

 * Python 2.6+ or 3.0+

If you would like to use testtools for earlier Python's, please use testtools
0.9.15.

 * extras (helpers that we intend to push into Python itself in the near
   future).


Optional Dependencies
---------------------

If you would like to use our undocumented, unsupported Twisted support, then
you will need Twisted.

If you want to use ``fixtures`` then you can either install fixtures (e.g. from
https://launchpad.net/python-fixtures or http://pypi.python.org/pypi/fixtures)
or alternatively just make sure your fixture objects obey the same protocol.


Bug reports and patches
-----------------------

Please report bugs using Launchpad at <https://bugs.launchpad.net/testtools>.
Patches should be submitted as Github pull requests, or mailed to the authors.
See ``doc/hacking.rst`` for more details.

There's no mailing list for this project yet, however the testing-in-python
mailing list may be a useful resource:

 * Address: testing-in-python@lists.idyll.org
 * Subscription link: http://lists.idyll.org/listinfo/testing-in-python


History
-------

testtools used to be called 'pyunit3k'.  The name was changed to avoid
conflating the library with the Python 3.0 release (commonly referred to as
'py3k').


Thanks
------

 * Canonical Ltd
 * Bazaar
 * Twisted Matrix Labs
 * Robert Collins
 * Andrew Bennetts
 * Benjamin Peterson
 * Jamu Kakar
 * James Westby
 * Martin [gz]
 * Michael Hudson-Doyle
 * Aaron Bentley
 * Christian Kampka
 * Gavin Panella
 * Martin Pool
