.. role:: bash(code)
   :language: bash

.. role:: js(code)
   :language: javascript

===============================
Guidelines for Fluent Reviewers
===============================

This document is intended as a guideline for developers and reviewers when
working with FTL (Fluent) files. As such, it’s not meant to replace the
`existing extensive documentation`__ about Fluent.

__ ./tutorial.html

`Herald`_ is used to set the group `fluent-reviewers`_ as blocking reviewer for
any patch modifying FTL files committed to Phabricator. The person from this
group performing the review will have to manually set other reviewers as
blocking, if the original developer didn’t originally do it.


.. hint::

  In case of doubt, you should always reach out to the l10n team for
  clarifications.


Message Identifiers
===================

While in Fluent it’s possible to use both lowercase and uppercase characters in
message identifiers, the naming convention in Gecko is to use lowercase and
hyphens (*kebab-case*), avoiding CamelCase and underscores. For example,
:js:`allow-button` should be preferred to :js:`allow_button` or
:js:`allowButton`, unless there are technically constraints – like identifiers
generated at run-time from external sources – that make this impractical.

When importing multiple FTL files, all messages share the same scope in the
Fluent bundle. For that reason, it’s suggested to add scope to the message
identifier itself: using :js:`cancel` as an identifier increases the chances of
having a conflict, :js:`save-dialog-cancel-button` would make it less likely.

Message identifiers are also used as the ultimate fall back in case of run-time
errors. Having a descriptive message ID would make such fall back more useful
for the user.

Comments
========

When a message includes placeables (variables), there should always be a
comment explaining the format of the variable, and what kind of content it will
be replaced with. This is the format suggested for such comments:


.. code-block:: fluent

  # This string is used on a new line below the add-on name
  # Variables:
  #   $name (String) - Add-on author name
  cfr-doorhanger-extension-author = by { $name }


By default, a comment is bound to the message immediately following it. Fluent
supports both `file-level and group-level comments`__. Be aware that a group
comment will apply to all messages following that comment until the end of the
file. If that shouldn’t be the case, you’ll need to “reset” the group comment,
by adding an empty one (:js:`##`), or moving the section of messages at the end
of the file.

__ https://projectfluent.org/fluent/guide/comments.html

Comments are fundamental for localizers, since they don’t see the file as a
whole, or changes as a fragment of a larger patch. Their work happens on a
message at a time, and the context is only provided by comments.

License headers are standalone comments, that is, a single :js:`#` as prefix,
and the comment is followed by at least one empty line.

Changes to Existing Messages
============================

You must update the message identifier if:

- The meaning of the sentence has changed.
- You’re changing the morphology of the message, by adding or removing attributes.

Messages are identified in the entire localization toolchain by their ID. For
this reason, there’s no need to change attribute names.

If your changes are relevant only for English — for example, to correct a
typographical error or to make letter case consistent — then there is generally
no need to update the message identifier.

There is a grey area between needing a new ID or not. In some cases, it will be
necessary to look at all the existing translations to determine if a new ID
would be beneficial. You should always reach out to the l10n team in case of
doubt.

Changing the message ID will invalidate the existing translation, the new
message will be reported as missing in all tools, and localizers will have to
retranslate it. This is the only reliable method to ensure that localizers
update existing localizations, and run-time stop using obsolete translations.

You must also update all instances where that message identifier is used in the
source code, including localization comments.

Non-text Elements in Messages
=============================

When a message includes non text-elements – like anchors or images – make sure
that they have a :js:`data-l10n-name` associated to them. Additional
attributes, like the URL for an anchor or CSS classes, should not be exposed
for localization in the FTL file. More details can be found in `this page`__
dedicated to DOM overlays.

__ https://github.com/projectfluent/fluent.js/wiki/DOM-Overlays#text-level-elements

This information is not relevant if your code is using `fluent-react`_, where
DOM overlays `work differently`__.

__ https://github.com/projectfluent/fluent.js/wiki/React-Overlays

Message References
==================

Consider the following example:


.. code-block:: fluent

  newtab-search-box-search-the-web-text = Search the Web
  newtab-search-box-search-the-web-input =
      .placeholder = { newtab-search-box-search-the-web-text }
      .title = { newtab-search-box-search-the-web-text }


This might seem to reduce the work for localizers, but it actually doesn’t
help:

- A change to the referenced message (:js:`newtab-search-box-search-the-web-text`)
  would require a new ID also for all messages referencing it.
- Translation memory can help with matching text, not with message references.

On the other hand, this approach is helpful if, for example, you want to
reference another element of the UI in your message:


.. code-block:: fluent

  help-button = Help
  help-explanation = Click the { help-button} to access support


This enforces consistency and, if :js:`help-button` changes, all other messages
will need to be updated anyway.

Terms
=====

Fluent supports a specific type of message, called `term`_. Terms are similar
to regular messages but they can only be used as references in other messages.
They are best used to define vocabulary and glossary items which can be used
consistently across the localization of the entire product.

Terms are typically used for brand names, like :js:`Firefox` or :js:`Mozilla`:
it allows to have them in one easily identifiable place, and raise warnings
when a localization is not using them. It helps enforcing consistency and brand
protection. If you simply need to reference a message from another message, you
don’t need a term: cross references between messages are allowed, but they
should not be abused, as already described.

Variants and plurals
====================

Consider the following example:


.. code-block:: fluent

  items-selected =
      { $num ->
          [0] Select items.
          [one] One item selected.
         *[other] { $num } items selected.
      }


In this example, there’s no guarantee that all localizations will have this
variant covered, since variants are private by design. The correct approach for
the example would be to have a separate message for the :js:`0` case:


.. code-block:: fluent

  # Separate messages which serve different purposes.
  items-select = Select items
  # The default variant works for all values of the selector.
  items-selected =
      { $num ->
          [one] One item selected.
         *[other] { $num } items selected.
      }


As a rule of thumb:

- Use variants only if the default variant makes sense for all possible values
  of the selector.
- The code shouldn’t depend on the availability of a specific variant.

More examples about selector and variant abuses can be found in `this wiki`__.

__ https://github.com/projectfluent/fluent/wiki/Good-Practices-for-Developers#prefer-separate-messages-over-variants-for-ui-logic

In general, also avoid putting a selector in the middle of a sentence, like in
the example below:


.. code-block:: fluent

  items-selected =
      { $num ->
          [one] One item.
         *[other] { $num } items
      } selected.


:js:`1` should only be used in case you want to cover the literal number. If
it’s a standard plural, you should use the :js:`one` category for singular.
Also make sure to always pass the variable to these messages as a number, not
as a string.

Access Keys
===========

The following is a simple potential example of an access key:

.. code-block:: fluent

  example-menu-item =
      .label = Menu Item
      .accesskey = M

Access keys are used in menus in order to help provide easy keyboard shortcut access. They
are useful for both power users, and for users who have accessibility needs. It is
helpful to first read the `Access keys`__ guide in the Windows Developer documentation,
as it outlines the best practices for Windows applications.

__ https://docs.microsoft.com/en-us/windows/uwp/design/input/access-keys

There are some differences between operating systems. Linux mostly follows the same
practices as Windows. However, macOS in general does not have good support for accesskeys,
especially in menus.

When choosing an access key, it's important that it's unique relative to the current level
of UI. It's preferable to avoid letters with descending parts, such as :code:`g`,
:code:`j`, :code:`p`, and :code:`q` as these will not be underlined nicely in Windows or
Linux. Other problematic characters are ones which are narrow, such as :code:`l`,
:code:`i` and :code:`I`. The underline may not be as visible as other letters in
sans-serif fonts.

Linter
======

:bash:`mach lint` includes a :ref:`l10n linter <L10n>`, called :bash:`moz-l10n-lint`. It
can be run locally by developers but also runs on Treeherder: in the Build
Status section of the diff on Phabricator, open the Treeherder Jobs link and
look for the :js:`l1nt` job.

Besides displaying errors and warnings due to syntax errors, it’s particularly
important because it also checks for message changes without new IDs, and
conflicts with the cross-channel repository used to ship localized versions of
Firefox.


.. warning::

  Currently, there’s an `issue`__ preventing warnings to be displayed in
  Phabricator. Checks can be run locally using :bash:`./mach lint -l l10n -W`.

  __ https://github.com/mozilla/code-review/issues/32


Migrating Strings From Legacy or Fluent Files
=============================================

If a patch is moving legacy strings (.properties, .DTD) to Fluent, it should
also include a recipe to migrate existing strings to FTL messages. The same is
applicable if a patch moves existing Fluent messages to a different file, or
changes the morphology of existing messages without actual changes to the
content.

Documentation on how to write and test migration recipes is available in `this
page`__.

__ ./fluent_migrations.html


.. _Herald: https://phabricator.services.mozilla.com/herald/
.. _fluent-reviewers: https://phabricator.services.mozilla.com/tag/fluent-reviewers/
.. _fluent-react: https://github.com/projectfluent/fluent.js/wiki/React-Bindings
.. _term: https://projectfluent.org/fluent/guide/terms.html
