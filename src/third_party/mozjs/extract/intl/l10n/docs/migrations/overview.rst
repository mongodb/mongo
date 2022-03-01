.. role:: bash(code)
   :language: bash

.. role:: js(code)
   :language: javascript

.. role:: python(code)
   :language: python

=====================================
Migration Recipes and Their Lifecycle
=====================================

The actual migrations are performed running Python modules called **migration
recipes**, which contain directives on how to migrate strings, which files are
involved, transformations to apply, etc. These recipes are stored in
`mozilla-central`__.

__ https://hg.mozilla.org/mozilla-central/file/default/python/l10n/fluent_migrations

When part of Firefox’s UI is migrated to Fluent, a migration recipe should be
attached to the same patch that adds new strings to `.ftl` files.

Migration recipes can quickly become obsolete, because the referenced strings
and files are removed from repositories as part of ongoing development.
For these reasons, l10n-drivers periodically clean up the `fluent_migrations`
folder in mozilla-central, keeping only recipes for 2
shipping versions (Nightly and Beta).


.. hint::

  As a developer you don’t need to bother about updating migration recipes
  already in `mozilla-central`: if a new patch removes a string or file that is
  used in a migration recipe, simply ignore it, since the entire recipe will be
  removed within a couple of cycles.


How to Write Migration Recipes
==============================

The migration recipe’s filename should start with a reference to the associated
bug number, and include a brief description of the bug, e.g.
:bash:`bug_1451992_preferences_applicationManager.py` is the migration recipe
used to migrate the Application Manager window in preferences. It’s also
possible to look at existing recipes in `mozilla-central`__ for inspiration.

__ https://hg.mozilla.org/mozilla-central/file/default/python/l10n/fluent_migrations


General Recipe Structure
========================

A migration recipe is a Python module, implementing the :py:func:`migrate`
function, which takes a :py:class:`MigrationContext` as input. The API provided
by the context is

.. code-block:: python

  class MigrationContext:
      def add_transforms(self, target, reference, transforms):
          """Define transforms for target using reference as template.

          `target` is a path of the destination FTL file relative to the
          localization directory. `reference` is a path to the template FTL
          file relative to the reference directory.

          Each transform is an extended FTL node with `Transform` nodes as some
          values.

          For transforms that merely copy legacy messages or Fluent patterns,
          using `fluent.migrate.helpers.transforms_from` is recommended.
          """

The skeleton of a migration recipe just implements the :py:func:`migrate`
function calling into :py:func:`ctx.add_transforms`, and looks like

.. code-block:: python

  # coding=utf8

  # Any copyright is dedicated to the Public Domain.
  # http://creativecommons.org/publicdomain/zero/1.0/

  from __future__ import absolute_import


  def migrate(ctx):
      """Bug 1552333 - Migrate feature to Fluent, part {index}"""
      target = 'browser/browser/feature.ftl'
      reference = 'browser/browser/feature.ftl'
      ctx.add_transforms(
          target,
          reference,
          [],  # Actual transforms go here.
      )

One can call into :py:func:`ctx.add_transforms` multiple times. In particular, one
can create migrated content in multiple files as part of a single migration
recipe by calling :py:func:`ctx.add_transforms` with different target-reference
pairs.

The *docstring* for this function will be used
as a commit message in VCS, that’s why it’s important to make sure the bug
reference is correct, and to keep the `part {index}` section: multiple strings
could have multiple authors, and would be migrated in distinct commits (part 1,
part 2, etc.).

Transforms
==========

The work of the migrations is done by the transforms that are passed as
last argument to :py:func:`ctx.add_transforms`. They're instances of either Fluent
:py:class:`fluent.syntax.ast.Message` or :py:class:`Term`, and their content
can depend on existing translation sources. The skeleton of a Message looks like

.. code-block:: python

    FTL.Message(
        id=FTL.Identifier(
            name="msg",
        ),
        value=FTL.Pattern(
            elements=[
                FTL.TextElement(
                    value="A string",
                ),
            ],
        ),
    )

When migrating existing legacy translations, you'll replace an
``FTL.TextElement`` with a ``COPY(legacy_path, "old_id")``, or one of its
variations we detail :doc:`next <legacy>`. When migrating existing Fluent
translations, an ``FTL.Pattern`` is replaced with a
``COPY_PATTERN(old_path, "old-id")``.
