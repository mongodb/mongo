.. role:: bash(code)
   :language: bash

.. role:: js(code)
   :language: javascript

.. role:: python(code)
   :language: python

===========================================
How Migrations Are Run on l10n Repositories
===========================================

Once a patch including new FTL strings and a migration recipe lands in
mozilla-central, l10n-drivers will perform a series of actions to migrate
strings in all 100+ localization repositories:

 - New Fluent strings land in `mozilla-central`, together with a migration
   recipe.
 - New strings are added to `gecko-strings-quarantine`_, a unified repository
   including strings for all shipping versions of Firefox, and used as a buffer
   before exposing strings to localizers.
 - Migration recipes are run against all l10n repositories, migrating strings
   from old to new files, and storing them in VCS.
 - New en-US strings are pushed to the official `gecko-strings`_ repository
   used by localization tools, and exposed to all localizers.

Migration recipes could be run again within a release cycle, in order to migrate
translations for legacy strings added after the first run. They’re usually
removed from `mozilla-central` within 2 cycles, e.g. a migration recipe created
for Firefox 59 would be removed when Firefox 61 is available in Nightly.


.. tip::

  A script to run migrations on all l10n repositories is available in `this
  repository`__, automating part of the steps described for manual testing, and
  it could be adapted to local testing.

  __ https://github.com/flodolo/fluent-migrations
.. _gecko-strings-quarantine: https://hg.mozilla.org/users/axel_mozilla.com/gecko-strings-quarantine
.. _gecko-strings: https://hg.mozilla.org/l10n/gecko-strings
