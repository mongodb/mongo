gecko-strings and Quarantine
============================

The actual generation is currently done manually on local machines. The state
that is good to use by localizers at large is published at
https://hg.mozilla.org/l10n/gecko-strings/.

The L10n team is doing a :ref:`review step <exposure-in-gecko-strings>` before publishing the strings, and while
that is ongoing, the intermediate state is published to 
https://hg.mozilla.org/users/axel_mozilla.com/gecko-strings-quarantine/.

The code is split up between some parts in
https://hg.mozilla.org/hgcustom/version-control-tools/  and others in
https://hg.mozilla.org/users/axel_mozilla.com/cross-channel-experimental/.
There's also `documentation`_ on how to run this code locally.

.. _documentation: https://cross-channel-experimental.readthedocs.io/en/latest/
