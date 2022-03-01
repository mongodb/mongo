.. role:: bash(code)
   :language: bash

.. role:: js(code)
   :language: javascript

.. role:: python(code)
   :language: python

=============================
How to Test Migration Recipes
=============================

To test migration recipes, use the following mach command:

.. code-block:: bash

  ./mach fluent-migration-test python/l10n/fluent_migrations/bug_1485002_newtab.py

This will analyze your migration recipe to check that the :python:`migrate`
function exists, and interacts correctly with the migration context. Once that
passes, it clones :bash:`gecko-strings` into :bash:`$OBJDIR/python/l10n`, creates a
reference localization by adding your local Fluent strings to the ones in
:bash:`gecko-strings`. It then runs the migration recipe, both as dry run and
as actual migration. Finally it analyzes the commits, and checks if any
migrations were actually run and the bug number in the commit message matches
the migration name.

It will also show the diff between the migrated files and the reference, ignoring
blank lines.

You can inspect the generated repository further by looking at

.. code-block:: bash

  ls $OBJDIR/python/l10n/bug_1485002_newtab/en-US

Caveats
-------

Be aware of hard-coded English context in migration. Consider for example:


.. code-block:: python

  ctx.add_transforms(
          "browser/browser/preferences/siteDataSettings.ftl",
          "browser/browser/preferences/siteDataSettings.ftl",
          transforms_from(
  """
  site-usage-persistent = { site-usage-pattern } (Persistent)
  """)
  )


This Transform will pass a manual comparison, since the two files are identical,
but will result in :js:`(Persistent)` being hard-coded in English for all
languages.
