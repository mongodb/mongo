.. role:: bash(code)
   :language: bash

.. role:: js(code)
   :language: javascript

.. role:: python(code)
   :language: python

========================
Migrating Legacy Formats
========================

Migrating from legacy formats is different from migrating Fluent to Fluent.
When migrating legacy code paths, you'll need to adjust the Fluent strings
for the quirks Mozilla uses in the legacy code paths. You'll find a number
of specialized functionalities here.

Basic Migration
---------------

Let’s consider a basic example: one string needs to be migrated, without
any further change, from a DTD file to Fluent.

The legacy string is stored in :bash:`toolkit/locales/en-US/chrome/global/findbar.dtd`:


.. code-block:: dtd

  <!ENTITY next.tooltip "Find the next occurrence of the phrase">


The new Fluent string is stored in :bash:`toolkit/locales/en-US/toolkit/main-window/findbar.ftl`:


.. code-block:: properties

  findbar-next =
      .tooltiptext = Find the next occurrence of the phrase


This is how the migration recipe looks:


.. code-block:: python

  # Any copyright is dedicated to the Public Domain.
  # http://creativecommons.org/publicdomain/zero/1.0/

  from __future__ import absolute_import
  import fluent.syntax.ast as FTL
  from fluent.migrate.helpers import transforms_from

  def migrate(ctx):
      """Bug 1411707 - Migrate the findbar XBL binding to a Custom Element, part {index}."""

      ctx.add_transforms(
          "toolkit/toolkit/main-window/findbar.ftl",
          "toolkit/toolkit/main-window/findbar.ftl",
          transforms_from(
  """
  findbar-next =
      .tooltiptext = { COPY(from_path, "next.tooltip") }
  """, from_path="toolkit/chrome/global/findbar.dtd"))


The first important thing to notice is that the migration recipe needs file
paths relative to a localization repository, losing :bash:`locales/en-US/`:

 - :bash:`toolkit/locales/en-US/chrome/global/findbar.dtd` becomes
   :bash:`toolkit/chrome/global/findbar.dtd`.
 - :bash:`toolkit/locales/en-US/toolkit/main-window/findbar.ftl` becomes
   :bash:`toolkit/toolkit/main-window/findbar.ftl`.

The :python:`context.add_transforms` function takes 3 arguments:

 - Path to the target l10n file.
 - Path to the reference (en-US) file.
 - An array of Transforms. Transforms are AST nodes which describe how legacy
   translations should be migrated.

.. note::

   For migrations of Firefox localizations, the target and reference path
   are the same. This isn't true for all projects that use Fluent, so both
   arguments are required.

In this case there is only one Transform that migrates the string with ID
:js:`next.tooltip` from :bash:`toolkit/chrome/global/findbar.dtd`, and injects
it in the FTL fragment. The :python:`COPY` Transform allows to copy the string
from an existing file as is, while :python:`from_path` is used to avoid
repeating the same path multiple times, making the recipe more readable. Without
:python:`from_path`, this could be written as:


.. code-block:: python

  ctx.add_transforms(
      "toolkit/toolkit/main-window/findbar.ftl",
      "toolkit/toolkit/main-window/findbar.ftl",
      transforms_from(
  """
  findbar-next =
      .tooltiptext = { COPY("toolkit/chrome/global/findbar.dtd", "next.tooltip") }
  """))


This method of writing migration recipes allows to take the original FTL
strings, and simply replace the value of each message with a :python:`COPY`
Transform. :python:`transforms_from` takes care of converting the FTL syntax
into an array of Transforms describing how the legacy translations should be
migrated. This manner of defining migrations is only suitable to simple strings
where a copy operation is sufficient. For more complex use-cases which require
some additional logic in Python, it’s necessary to resort to the raw AST.


The example above is equivalent to the following syntax, which exposes
the underlying AST structure:


.. code-block:: python

  ctx.add_transforms(
      "toolkit/toolkit/main-window/findbar.ftl",
      "toolkit/toolkit/main-window/findbar.ftl",
      [
          FTL.Message(
              id=FTL.Identifier("findbar-next"),
              attributes=[
                  FTL.Attribute(
                      id=FTL.Identifier("tooltiptext"),
                      value=COPY(
                          "toolkit/chrome/global/findbar.dtd",
                          "next.tooltip"
                      )
                  )
              ]
          )
      ]
  )

This creates a :python:`Message`, taking the value from the legacy string
:js:`findbar-next`. A message can have an array of attributes, each with an ID
and a value: in this case there is only one attribute, with ID :js:`tooltiptext`
and :js:`value` copied from the legacy string.

Notice how both the ID of the message and the ID of the attribute are
defined as an :python:`FTL.Identifier`, not simply as a string.


.. tip::

  It’s possible to concatenate arrays of Transforms defined manually, like in
  the last example, with those coming from :python:`transforms_from`, by using
  the :python:`+` operator. Alternatively, it’s possible to use multiple
  :python:`add_transforms`.

  The order of Transforms provided in the recipe is not relevant, the reference
  file is used for ordering messages.


Replacing Content in Legacy Strings
-----------------------------------

While :python:`COPY` allows to copy a legacy string as is, :python:`REPLACE`
(from `fluent.migrate`) allows to replace content while performing the
migration. This is necessary, for example, when migrating strings that include
placeholders or entities that need to be replaced to adapt to Fluent syntax.

Consider for example the following string:


.. code-block:: DTD

  <!ENTITY aboutSupport.featuresTitle "&brandShortName; Features">


Which needs to be migrated to:


.. code-block:: fluent

  features-title = { -brand-short-name } Features


The entity :js:`&brandShortName;` needs to be replaced with a term reference:


.. code-block:: python

  FTL.Message(
      id=FTL.Identifier("features-title"),
      value=REPLACE(
          "toolkit/chrome/global/aboutSupport.dtd",
          "aboutSupport.featuresTitle",
          {
              "&brandShortName;": TERM_REFERENCE("brand-short-name"),
          },
      )
  ),


This creates an :python:`FTL.Message`, taking the value from the legacy string
:js:`aboutSupport.featuresTitle`, but replacing the specified text with a
Fluent term reference.

.. note::
  :python:`REPLACE` replaces all occurrences of the specified text.


It’s also possible to replace content with a specific text: in that case, it
needs to be defined as a :python:`TextElement`. For example, to replace
:js:`example.com` with HTML markup:


.. code-block:: python

  value=REPLACE(
      "browser/chrome/browser/preferences/preferences.properties",
      "searchResults.sorryMessageWin",
      {
          "example.com": FTL.TextElement('<span data-l10n-name="example"></span>')
      }
  )


The situation is more complex when a migration recipe needs to replace
:js:`printf` arguments like :js:`%S`. In fact, the format used for localized
and source strings doesn’t need to match, and the two following strings using
unordered and ordered argument are perfectly equivalent:


.. code-block:: properties

  btn-quit = Quit %S
  btn-quit = Quit %1$S


In this scenario, replacing :js:`%S` would work on the first version, but not
on the second, and there’s no guarantee that the localized string uses the
same format as the source string.

Consider also the following string that uses :js:`%S` for two different
variables, implicitly relying on the order in which the arguments appear:


.. code-block:: properties

  updateFullName = %S (%S)


And the target Fluent string:


.. code-block:: fluent

  update-full-name = { $name } ({ $buildID })


As indicated, :python:`REPLACE` would replace all occurrences of :js:`%S`, so
only one variable could be set. The string needs to be normalized and treated
like:


.. code-block:: properties

  updateFullName = %1$S (%2$S)


This can be obtained by calling :python:`REPLACE` with
:python:`normalize_printf=True`:


.. code-block:: python

  FTL.Message(
      id=FTL.Identifier("update-full-name"),
      value=REPLACE(
          "toolkit/chrome/mozapps/update/updates.properties",
          "updateFullName",
          {
              "%1$S": VARIABLE_REFERENCE("name"),
              "%2$S": VARIABLE_REFERENCE("buildID"),
          },
          normalize_printf=True
      )
  )


.. attention::

  To avoid any issues :python:`normalize_printf=True` should always be used when
  replacing :js:`printf` arguments. This is the default behaviour when working
  with .properties files.

.. note::

  :python:`VARIABLE_REFERENCE`, :python:`MESSAGE_REFERENCE`, and
  :python:`TERM_REFERENCE` are helper Transforms which can be used to save
  keystrokes in common cases where using the raw AST is too verbose.

  :python:`VARIABLE_REFERENCE` is used to create a reference to a variable, e.g.
  :js:`{ $variable }`.

  :python:`MESSAGE_REFERENCE` is used to create a reference to another message,
  e.g. :js:`{ another-string }`.

  :python:`TERM_REFERENCE` is used to create a reference to a `term`__,
  e.g. :js:`{ -brand-short-name }`.

  Both Transforms need to be imported at the beginning of the recipe, e.g.
  :python:`from fluent.migrate.helpers import VARIABLE_REFERENCE`

  __ https://projectfluent.org/fluent/guide/terms.html


Trimming Unnecessary Whitespaces in Translations
------------------------------------------------

.. note::

  This section was updated in May 2020 to reflect the change to the default
  behavior: legacy translations are now trimmed, unless the :python:`trim`
  parameter is set explicitly.

It’s not uncommon to have strings with unnecessary leading or trailing spaces
in legacy translations. These are not meaningful, don’t have practical results
on the way the string is displayed in products, and are added mostly for
formatting reasons. For example, consider this DTD string:


.. code-block:: DTD

  <!ENTITY aboutAbout.note   "This is a list of “about” pages for your convenience.<br/>
                              Some of them might be confusing. Some are for diagnostic purposes only.<br/>
                              And some are omitted because they require query strings.">


By default, the :python:`COPY`, :python:`REPLACE`, and :python:`PLURALS`
transforms will strip the leading and trailing whitespace from each line of the
translation, as well as the empty leading and trailing lines. The above string
will be migrated as the following Fluent message, despite copious indentation
on the second and the third line in the original:


.. code-block:: fluent

  about-about-note =
      This is a list of “about” pages for your convenience.<br/>
      Some of them might be confusing. Some are for diagnostic purposes only.<br/>
      And some are omitted because they require query strings.


To disable the default trimming behavior, set :python:`trim:"False"` or
:python:`trim=False`, depending on the context:


.. code-block:: python

  transforms_from(
  """
  about-about-note = { COPY("toolkit/chrome/global/aboutAbout.dtd", "aboutAbout.note", trim:"False") }
  """)

  FTL.Message(
      id=FTL.Identifier("discover-description"),
      value=REPLACE(
          "toolkit/chrome/mozapps/extensions/extensions.dtd",
          "discover.description2",
          {
              "&brandShortName;": TERM_REFERENCE("-brand-short-name")
          },
          trim=False
      )
  ),


Concatenating Strings
---------------------

It's best practice to only expose complete phrases to localization, and to avoid
stitching localized strings together in code. With `DTD` and `properties`,
there were few options. So when migrating to Fluent, you'll find
it quite common to concatenate multiple strings coming from `DTD` and
`properties`, for example to create sentences with HTML markup. It’s possible to
concatenate strings and text elements in a migration recipe using the
:python:`CONCAT` Transform.

Note that in case of simple migrations using :python:`transforms_from`, the
concatenation is carried out implicitly by using the Fluent syntax interleaved
with :python:`COPY()` transform calls to define the migration recipe.

Consider the following example:


.. code-block:: properties

  # %S is replaced by a link, using searchResults.needHelpSupportLink as text
  searchResults.needHelp = Need help? Visit %S

  # %S is replaced by "Firefox"
  searchResults.needHelpSupportLink = %S Support


In Fluent:


.. code-block:: fluent

  search-results-need-help-support-link = Need help? Visit <a data-l10n-name="url">{ -brand-short-name } Support</a>


This is quite a complex migration: it requires to take 2 legacy strings, and
concatenate their values with HTML markup. Here’s how the Transform is defined:


.. code-block:: python

  FTL.Message(
      id=FTL.Identifier("search-results-help-link"),
      value=REPLACE(
          "browser/chrome/browser/preferences/preferences.properties",
          "searchResults.needHelp",
          {
              "%S": CONCAT(
                  FTL.TextElement('<a data-l10n-name="url">'),
                  REPLACE(
                      "browser/chrome/browser/preferences/preferences.properties",
                      "searchResults.needHelpSupportLink",
                      {
                          "%1$S": TERM_REFERENCE("brand-short-name"),
                      },
                      normalize_printf=True
                  ),
                  FTL.TextElement("</a>")
              )
          }
      )
  ),


:js:`%S` in :js:`searchResults.needHelpSupportLink` is replaced by a reference
to the term :js:`-brand-short-name`, migrating from :js:`%S Support` to :js:`{
-brand-short-name } Support`. The result of this operation is then inserted
between two text elements to create the anchor markup. The resulting text is
finally  used to replace :js:`%S` in :js:`searchResults.needHelp`, and used as
value for the FTL message.


.. important::

  When concatenating existing strings, avoid introducing changes to the original
  text, for example adding spaces or punctuation. Each language has its own
  rules, and this might result in poor migrated strings. In case of doubt,
  always ask for feedback.


When more than 1 element is passed in to concatenate, :python:`CONCAT`
disables whitespace trimming described in the section above on all legacy
Transforms passed into it: :python:`COPY`, :python:`REPLACE`, and
:python:`PLURALS`, unless the :python:`trim` parameters has been set
explicitly on them. This helps ensure that spaces around segments are not
lost during the concatenation.

When only a single element is passed into :python:`CONCAT`, however, the
trimming behavior is not altered, and follows the rules described in the
previous section. This is meant to make :python:`CONCAT(COPY())` equivalent
to a bare :python:`COPY()`.


Plural Strings
--------------

Migrating plural strings from `.properties` files usually involves two
Transforms from :python:`fluent.migrate.transforms`: the
:python:`REPLACE_IN_TEXT` Transform takes TextElements as input, making it
possible to pass it as the foreach function of the :python:`PLURALS` Transform.

Consider the following legacy string:


.. code-block:: properties

  # LOCALIZATION NOTE (disableContainersOkButton): Semi-colon list of plural forms.
  # See: http://developer.mozilla.org/en/docs/Localization_and_Plurals
  # #1 is the number of container tabs
  disableContainersOkButton = Close #1 Container Tab;Close #1 Container Tabs


In Fluent:


.. code-block:: fluent

  containers-disable-alert-ok-button =
      { $tabCount ->
          [one] Close { $tabCount } Container Tab
         *[other] Close { $tabCount } Container Tabs
      }


This is how the Transform for this string is defined:


.. code-block:: python

  FTL.Message(
      id=FTL.Identifier("containers-disable-alert-ok-button"),
      value=PLURALS(
          "browser/chrome/browser/preferences/preferences.properties",
          "disableContainersOkButton",
          VARIABLE_REFERENCE("tabCount"),
          lambda text: REPLACE_IN_TEXT(
              text,
              {
                  "#1": VARIABLE_REFERENCE("tabCount")
              }
          )
      )
  )


The `PLURALS` Transform will take care of creating the correct number of plural
categories for each language. Notice how `#1` is replaced for each of these
variants with :js:`{ $tabCount }`, using :python:`REPLACE_IN_TEXT` and
:python:`VARIABLE_REFERENCE("tabCount")`.

In this case it’s not possible to use :python:`REPLACE` because it takes a file
path and a message ID as arguments, whereas here the recipe needs to operate on
regular text. The replacement is performed on each plural form of the original
string, where plural forms are separated by a semicolon.

Explicit Variants
-----------------

Explicitly creating variants of a string is useful for platform-dependent
terminology, but also in cases where you want a one-vs-many split of a string.
It’s always possible to migrate strings by manually creating the underlying AST
structure. Consider the following complex Fluent string:


.. code-block:: fluent

  use-current-pages =
      .label =
          { $tabCount ->
              [1] Use Current Page
             *[other] Use Current Pages
          }
      .accesskey = C


The migration for this string is quite complex: the :js:`label` attribute is
created from 2 different legacy strings, and it’s not a proper plural form.
Notice how the first string is associated to the :js:`1` case, not the :js:`one`
category used in plural forms. For these reasons, it’s not possible to use
:python:`PLURALS`, the Transform needs to be crafted recreating the AST.


.. code-block:: python


  FTL.Message(
      id=FTL.Identifier("use-current-pages"),
      attributes=[
          FTL.Attribute(
              id=FTL.Identifier("label"),
              value=FTL.Pattern(
                  elements=[
                      FTL.Placeable(
                          expression=FTL.SelectExpression(
                              selector=VARIABLE_REFERENCE("tabCount"),
                              variants=[
                                  FTL.Variant(
                                      key=FTL.NumberLiteral("1"),
                                      default=False,
                                      value=COPY(
                                          "browser/chrome/browser/preferences/main.dtd",
                                          "useCurrentPage.label",
                                      )
                                  ),
                                  FTL.Variant(
                                      key=FTL.Identifier("other"),
                                      default=True,
                                      value=COPY(
                                          "browser/chrome/browser/preferences/main.dtd",
                                          "useMultiple.label",
                                      )
                                  )
                              ]
                          )
                      )
                  ]
              )
          ),
          FTL.Attribute(
              id=FTL.Identifier("accesskey"),
              value=COPY(
                  "browser/chrome/browser/preferences/main.dtd",
                  "useCurrentPage.accesskey",
              )
          ),
      ],
  ),


This Transform uses several concepts already described in this document. Notable
is the :python:`SelectExpression` inside a :python:`Placeable`, with an array
of :python:`Variant` objects. Exactly one of those variants needs to have
``default=True``.

This example can still use :py:func:`transforms_from()``, since existing strings
are copied without interpolation.

.. code-block:: python

  transforms_from(
  """
  use-current-pages =
      .label =
          { $tabCount ->
              [1] { COPY(main_dtd, "useCurrentPage.label") }
             *[other] { COPY(main_dtd, "useMultiple.label") }
          }
      .accesskey = { COPY(main_dtd, "useCurrentPage.accesskey") }
  """, main_dtd="browser/chrome/browser/preferences/main.dtd"
  )
