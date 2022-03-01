.. role:: bash(code)
   :language: bash

.. role:: js(code)
   :language: javascript

.. role:: python(code)
   :language: python

=============================================
Migrating Strings From Legacy or Fluent Files
=============================================

Firefox is a project localized in over 100 languages. As the code for existing
features moves away from the old localization systems and starts using
`Fluent`_, we need to ensure that we don’t lose existing translations, which
would have the adverse effect of forcing contributors to localize hundreds of
strings from scratch.

`Fluent Migration`_ is a Python library designed to solve this specific problem:
it allows to migrate legacy translations from `.dtd` and `.properties` files,
not only moving strings and transforming them as needed to adapt to the `FTL`
syntax, but also replicating "blame" for each string in VCS.

The library also includes basic support for migrating existing Fluent messages
without interpolations (e.g. variable replacements). The typical use cases
would be messages moving as-is to a different file, or changes to the
morphology of existing messages (e.g move content from an attribute to the
value of the message).

.. toctree::
  :maxdepth: 2

  overview
  legacy
  fluent
  testing
  localizations

How to Get Help
===============

Writing migration recipes can be challenging for non trivial cases, and it can
require extensive l10n knowledge to avoid localizability issues.

Don’t hesitate to reach out to the l10n-drivers for feedback, help to test or
write the migration recipes:

 - Francesco Lodolo (:flod)
 - Staś Małolepszy (:stas)
 - Zibi Braniecki (:gandalf)
 - Axel Hecht (:pike)

.. _Fluent: http://projectfluent.org/
.. _Fluent Migration: https://hg.mozilla.org/l10n/fluent-migration/
