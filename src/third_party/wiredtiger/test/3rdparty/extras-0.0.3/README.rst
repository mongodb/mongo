======
extras
======

extras is a set of extensions to the Python standard library, originally
written to make the code within testtools cleaner, but now split out for
general use outside of a testing context.


Documentation
-------------

pydoc extras is your friend. extras currently contains the following functions:

* try_import

* try_imports

* safe_hasattr

Which do what their name suggests.


Licensing
---------

This project is distributed under the MIT license and copyright is owned by
the extras authors. See LICENSE for details.


Required Dependencies
---------------------

 * Python 2.6+ or 3.0+


Bug reports and patches
-----------------------

Please report bugs using github issues at <https://github.com/testing-cabal/extras>.
Patches can also be submitted via github.  You can mail the authors directly
via the mailing list testtools-dev@lists.launchpad.net. (Note that Launchpad
discards email from unknown addresses - be sure to sign up for a Launchpad
account before mailing the list, or your mail will be silently discarded).


History
-------

extras used to be testtools.helpers, and was factored out when folk wanted to
use it separately.


Thanks
------

 * Martin Pool
