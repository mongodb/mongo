.. role:: html(code)
   :language: html

.. role:: js(code)
   :language: javascript

=============================
Fluent for Firefox Developers
=============================


This tutorial is intended for Firefox engineers already familiar with the previous
localization systems offered by Gecko - `DTD`_ and  `StringBundle`_ - and assumes
prior experience with those systems.


Using Fluent in Gecko
=====================

`Fluent`_ is a modern localization system introduced into
the Gecko platform with a focus on quality, performance, maintenance and completeness.

The legacy DTD system is deprecated, and Fluent should be used where possible.

Getting a Review
----------------

If you work on any patch that touches FTL files, you'll need to get a review
from `fluent-reviewers`__. There's a Herald hook that automatically sets
that group as a blocking reviewer.

__ https://phabricator.services.mozilla.com/tag/fluent-reviewers/

Guidelines for the review process are available `here`__.

__ ./fluent_review.html

Major Benefits
==============

Fluent `ties tightly`__ into the domain of internationalization
through `Unicode`_, `CLDR`_ and `ICU`_.

__ https://github.com/projectfluent/fluent/wiki/Fluent-and-Standards

More specifically, the most observable benefits for each group of consumers are


Developers
----------

 - Support for XUL, XHTML, HTML, Web Components, React, JS, Python and Rust
 - Strings are available in a single, unified localization context available for both DOM and runtime code
 - Full internationalization (i18n) support: date and time formatting, number formatting, plurals, genders etc.
 - Strong focus on `declarative API via DOM attributes`__
 - Extensible with custom formatters, Mozilla-specific APIs etc.
 - `Separation of concerns`__: localization details, and the added complexity of some languages, don't leak onto the source code and are no concern for developers
 - Compound messages link a single translation unit to a single UI element
 - `DOM Overlays`__ allow for localization of DOM fragments
 - Simplified build system model
 - No need for pre-processing instructions
 - Support for pseudolocalization

__ https://github.com/projectfluent/fluent/wiki/Get-Started
__ https://github.com/projectfluent/fluent/wiki/Design-Principles
__ https://github.com/projectfluent/fluent.js/wiki/DOM-Overlays


Product Quality
------------------

 - A robust, multilevel, `error fallback system`__ prevents XML errors and runtime errors
 - Simplified l10n API reduces the amount of l10n specific code and resulting bugs
 - Runtime localization allows for dynamic language changes and updates over-the-air
 - DOM Overlays increase localization security

__ https://github.com/projectfluent/fluent/wiki/Error-Handling


Fluent Translation List - FTL
=============================

Fluent introduces a file format designed specifically for easy readability
and the localization features offered by the system.

At first glance the format is a simple key-value store. It may look like this:

.. code-block:: fluent

  home-page-header = Home Page

  # The label of a button opening a new tab
  new-tab-open = Open New Tab

But the FTL file format is significantly more powerful and the additional features
quickly add up. In order to familiarize yourself with the basic features,
consider reading through the `Fluent Syntax Guide`_ to understand
a more complex example like:

.. code-block:: fluent

  ### These messages correspond to security and privacy user interface.
  ###
  ### Please choose simple and non-threatening language when localizing
  ### to help user feel in control when interacting with the UI.

  ## General Section

  -brand-short-name = Firefox
      .gender = masculine
  
  pref-pane =
      .title =
          { PLATFORM() ->
              [windows] Options
             *[other] Preferences
          }
      .accesskey = C
  
  # Variables:
  #   $tabCount (Number) - number of container tabs to be closed
  containers-disable-alert-ok-button =
      { $tabCount ->
          [one] Close { $tabCount } Container Tab
         *[other] Close { $tabCount } Container Tabs
      }
  
  update-application-info =
      You are using { -brand-short-name } Version: { $version }.
      Please read the <a>privacy policy</a>.

The above, of course, is a particular selection of complex strings intended to exemplify
the new features and concepts introduced by Fluent.

.. important::

  While in Fluent it’s possible to use both lowercase and uppercase characters in message
  identifiers, the naming convention in Gecko is to use lowercase and hyphens, avoiding
  CamelCase and underscores. For example, `allow-button` should be preferred to
  `allow_button` or `allowButton`, unless there are technically constraints – like
  identifiers generated at run-time from external sources – that make this impractical.

In order to ensure the quality of the output, a lot of checks and tooling
is part of the build system.
`Pontoon`_, the main localization tool used to translate Firefox, also supports
Fluent and its features to help localizers in their work.


.. _fluent-tutorial-social-contract:

Social Contract
===============

Fluent uses the concept of a `social contract` between developer and localizers.
This contract is established by the selection of a unique identifier, called :js:`l10n-id`,
which carries a promise of being used in a particular place to carry a particular meaning.

The use of unique identifiers is shared with legacy localization systems in
Firefox.

.. important::

  An important part of the contract is that the developer commits to treat the
  localization output as `opaque`. That means that no concatenations, replacements
  or splitting should happen after the translation is completed to generate the
  desired output.

In return, localizers enter the social contract by promising to provide an accurate
and clean translation of the messages that match the request.

In Fluent, the developer is not to be bothered with inner logic and complexity that the
localization will use to construct the response. Whether `declensions`__ or other
variant selection techniques are used is up to a localizer and their particular translation.
From the developer perspective, Fluent returns a final string to be presented to
the user, with no l10n logic required in the running code.

__ https://en.wikipedia.org/wiki/Declension


Markup Localization
===================

To localize an element in Fluent, the developer adds a new message to
an FTL file and then has to associate an :js:`l10n-id` with the element
by defining a :js:`data-l10n-id` attribute:

.. code-block:: html

  <h1 data-l10n-id="home-page-header" />

  <button data-l10n-id="pref-pane" />

Fluent will take care of the rest, populating the element with the message value
in its content and all localizable attributes if defined.

The developer provides only a single message to localize the whole element,
including the value and selected attributes.

The value can be a whole fragment of DOM:

.. code-block:: html

  <p data-l10n-id="update-application-info" data-l10n-args='{"version": "60.0"}'>
    <a data-l10n-name="privacy-url" href="http://www.mozilla.org/privacy" />
  </p>

.. code-block:: fluent

  -brand-short-name = Firefox
  update-application-info =
      You are using { -brand-short-name } Version: { $version }.
      Please read the <a data-l10n-name="privacy-url">privacy policy</a>.


Fluent will overlay the translation onto the source fragment preserving attributes like
:code:`class` and :code:`href` from the source and adding translations for the elements
inside. The resulting localized content will look like this:

.. code-block:: html

  <p data-l10n-id="update-application-info" data-l10n-args='{"version": "60.0"}'">
    You are using Firefox Version: 60.0.
    Please read the <a href="http://www.mozilla.org/privacy">privacy policy</a>.
  </p>


This operation is sanitized, and Fluent takes care of selecting which elements and
attributes can be safely provided by the localization.
The list of allowed elements and attributes is `maintained by the W3C`__, and if
the developer needs to allow for localization of additional attributes, they can
allow them using :code:`data-l10n-attrs` list:

.. code-block:: html

  <label data-l10n-id="search-input" data-l10n-attrs="style" />

The above example adds an attribute :code:`style` to be allowed on this
particular :code:`label` element.


External Arguments
------------------

Notice in the previous example the attribute :code:`data-l10n-args`, which is
a JSON object storing variables exposed by the developer to the localizer.

This is the main channel for the developer to provide additional variables
to be used in the localization.

Arguments are rarely needed for situations where it’s currently possible to use
DTD, since such variables would need to be computed from the code at runtime.
It's worth noting that, when the :code:`l10n-args` are set in
the runtime code, they are in fact encoded as JSON and stored together with
:code:`l10n-id` as an attribute of the element.

__ https://www.w3.org/TR/2011/WD-html5-20110525/text-level-semantics.html


Runtime Localization
====================

In almost every case the JS runtime code will operate on a particular document, either
XUL, XHTML or HTML.

If the document has its markup already localized, then Fluent exposes a new
attribute on the :js:`document` element - :js:`document.l10n`.

This property is an object of type :js:`DOMLocalization` which maintains the main
localization context for this document and exposes it to runtime code as well.

With a focus on `declarative localization`__, the primary method of localization is
to alter the localization attributes in the DOM. Fluent provides a method to facilitate this:

.. code-block:: javascript

  document.l10n.setAttributes(element, "new-panel-header");

This will set the :code:`data-l10n-id` on the element and translate it before the next
animation frame.

The reason to use this API over manually setting the attribute is that it also
facilitates encoding l10n arguments as JSON:

.. code-block:: javascript

  document.l10n.setAttributes(element, "containers-disable-alert-ok-button", {
    tabCount: 5
  });

__ https://github.com/projectfluent/fluent/wiki/Good-Practices-for-Developers


Non-Markup Localization
-----------------------

In rare cases, when the runtime code needs to retrieve the translation and not
apply it onto the DOM, Fluent provides an API to retrieve it:

.. code-block:: javascript

  let [ msg ] = await document.l10n.formatValues([
    {id: "remove-containers-description"}
  ]);

  alert(msg);

This model is heavily discouraged and should be used only in cases where the
DOM annotation is not possible.

.. note::

  This API is available as asynchronous. In case of Firefox,
  the only non-DOM localizable calls are used where the output goes to
  a third-party like Bluetooth, Notifications etc.
  All those cases should already be asynchronous. If you can't avoid synchronous
  access, you can use ``mozILocalization.formatMessagesSync`` with synchronous IO.


Internationalization
====================

The majority of internationalization issues are implicitly handled by Fluent without
any additional requirement. Full Unicode support, `bidirectionality`__, and
correct number formatting work without any action required from either
developer or localizer.

__ https://github.com/projectfluent/fluent/wiki/BiDi-in-Fluent

.. code-block:: javascript

  document.l10n.setAttributes(element, "welcome-message", {
    userName: "اليسع",
    count: 5
  });

A message like this localized to American English will correctly wrap the user
name in directionality marks, allowing the layout engine to determine how to
display the bidirectional text.

On the other hand, the same message localized to Arabic will use the Eastern Arabic
numeral for number "5".


Plural Rules
------------

The most common localization feature is the ability to provide different variants
of the same string depending on plural categories. Fluent ties into the Unicode CLDR
standard called `Plural Rules`_.

In order to allow localizers to use it, all the developer has to do is to pass
an external argument number:

.. code-block:: javascript

  document.l10n.setAttributes(element, "unread-warning", { unreadCount: 5 });

Localizers can use the argument to build a multi variant message if their
language requires that:

.. code-block:: fluent

  unread-warning =
      { $unreadCount ->
          [one] You have { $unreadCount } unread message
         *[other] You have { $unreadCount } unread messages
      }

If the variant selection is performed based on a number, Fluent matches that
number against literal numbers as well as its `plural category`__.

If the given translation doesn't need pluralization for the string (for example
Japanese often will not), the localizer can replace it with:

.. code-block:: fluent

  unread-warning = You have { $unreadCount } unread messages

and the message will preserve the social contract.

One additional feature is that the localizer can further improve the message by
specifying variants for particular values:

.. code-block:: fluent

  unread-warning =
      { $unreadCount ->
          [0] You have no unread messages
          [1] You have one unread message
         *[other] You have { $unreadCount } unread messages
      }

The advantage here is that per-locale choices don't leak onto the source code
and the developer is not affected.


.. note::

  There is an important distinction between a variant keyed on plural category
  `one` and digit `1`. Although in English the two are synonymous, in other
  languages category `one` may be used for other numbers.
  For example in `Bosnian`__, category `one` is used for numbers like `1`, `21`, `31`
  and so on, and also for fractional numbers like `0.1`.

__ https://unicode.org/cldr/charts/latest/supplemental/language_plural_rules.html
__ https://unicode.org/cldr/charts/latest/supplemental/language_plural_rules.html#bs

Partially-formatted variables
-----------------------------

When it comes to formatting data, Fluent allows the developer to provide
a set of parameters for the formatter, and the localizer can fine tune some of them.
This technique is called `partially-formatted variables`__.

For example, when formatting a date, the developer can just pass a JS :js:`Date` object,
but its default formatting will be pretty expressive. In most cases, the developer
may want to use some of the :js:`Intl.DateTimeFormat` options to select the default
representation of the date in string:

.. code-block:: javascript

  document.l10n.formatValue("welcome-message", {
  startDate: FluentDateTime(new Date(), {
      year: "numeric",
      month: "long",
      day: "numeric"
    })
  });

.. code-block:: fluent

  welcome-message = Your session will start date: { $startDate }

In most cases, that will be enough and the date would get formatted in the current
Firefox as `February 28, 2018`.

But if in some other locale the string would get too long, the localizer can fine
tune the options as well:

.. code-block:: fluent

  welcome-message = Początek Twojej sesji: { DATETIME($startDate, month: "short") }

This will adjust the length of the month token in the message to short and get formatted
in Polish as `28 lut 2018`.

At the moment Fluent supports two formatters that match JS Intl API counterparts:

 * **NUMBER**: `Intl.NumberFormat`__
 * **DATETIME**: `Intl.DateTimeFormat`__

With time more formatters will be added. Also, this feature is not exposed
to ``setAttributes`` at this point, as that serializes to JSON.

__ https://projectfluent.org/fluent/guide/functions.html#partially-formatted-variables
__ https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/NumberFormat
__ https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/DateTimeFormat

Registering New L10n Files
==========================

Fluent uses a wildcard statement, packaging all localization resources into
their component's `/localization/` directory.

That means that, if a new file is added to a component of Firefox already
covered by Fluent like `browser`, it's enough to add the new file to the
repository in a path like `browser/locales/en-US/browser/component/file.ftl`, and
the toolchain will package it into `browser/localization/browser/component/file.ftl`.

At runtime Firefox uses a special registry for all localization data. It will
register the browser's `/localization/` directory and make all files inside it
available to be referenced.

To make the document localized using Fluent, all the developer has to do is add
localizable resources for Fluent API to use:

.. code-block:: html

  <link rel="localization" href="branding/brand.ftl"/>
  <link rel="localization" href="browser/preferences/preferences.ftl"/>

The URI provided to the :html:`<link/>` element are relative paths within the localization
system.


Custom Localizations
====================

The above method creates a single localization context per document.
In almost all scenarios that's sufficient.

In rare edge cases where the developer needs to fetch additional resources, or
the same resources in another language, it is possible to create additional
Localization object manually using the `Localization` class:

.. code-block:: javascript

  const { Localization } =
    ChromeUtils.import("resource://gre/modules/Localization.jsm", {});


  const myL10n = new Localization([
    "branding/brand.ftl",
    "browser/preferences/preferences.ftl"
  ]);


  let [isDefaultMsg, isNotDefaultMsg] =
    await myL10n.formatValues({id: "is-default"}, {id: "is-not-default"});


.. admonition:: Example

  An example of a use case is the Preferences UI in Firefox, which uses the
  main context to localize the UI but also to build a search index.

  It is common to build such search index both in a current language and additionally
  in English, since a lot of documentation and online help exist only in English.

  A developer may create manually a new context with the same resources as the main one,
  but hardcode it to `en-US` and then build the search index using both contexts.


By default, all `Localization` contexts are asynchronous. It is possible to create a synchronous
one by passing an `sync = false` argument to the constructor, or calling the `SetIsSync(bool)` method
on the class.


.. code-block:: javascript

  const { Localization } =
    ChromeUtils.import("resource://gre/modules/Localization.jsm", {});


  const myL10n = new Localization([
    "branding/brand.ftl",
    "browser/preferences/preferences.ftl"
  ], false);


  let [isDefaultMsg, isNotDefaultMsg] =
    myL10n.formatValuesSync({id: "is-default"}, {id: "is-not-default"});


Synchronous contexts should be always avoided as they require synchronous I/O. If you think your use case
requires a synchronous localization context, please consult Gecko, Performance and L10n Drivers teams.


Designing Localizable APIs
==========================

When designing localizable APIs, the most important rule is to resolve localization as
late as possible. That means that instead of resolving strings somewhere deep in the
codebase and then passing them on, or even caching, it is highly recommended to pass
around :code:`l10n-id` or :code:`[l10n-id, l10n-args]` pairs until the top-most code
resolves them or applies them onto the DOM element.


Testing
=======

When writing tests that involve both I18n and L10n, the general rule is that
result strings are opaque. That means that the developer should not assume any particular
value and should never test against it.

In case of raw i18n the :js:`resolvedOptions` method on all :js:`Intl.*` formatters
makes it relatively easy. In case of localization, the recommended way is to test that
the code sets the right :code:`l10n-id`/:code:`l10n-args` attributes like this:

.. code-block:: javascript
  
  testedFunction();
  
  const l10nAttrs = document.l10n.getAttributes(element);
  
  deepEquals(l10nAttrs, {
    id: "my-expected-id",
    args: {
      unreadCount: 5
    }
  });

If the code really has to test for particular values in the localized UI, it is
always better to scan for a variable:

.. code-block:: javascript

  testedFunction();
  
  equals(element.textContent.contains("John"));

.. important::

  Testing against whole values is brittle and will break when we insert Unicode
  bidirectionality marks into the result string or adapt the output in other ways.


Pseudolocalization
==================

When working with a Fluent-backed UI, the developer gets a new tool to test their UI
against several classes of problems.

Pseudolocalization is a mechanism which transforms messages on the fly, using
specific logic to help emulate how the UI will look once it gets localized.

The three classes of potential problems that this can help with are:

 - Hardcoded strings.

   Turning on pseudolocalization should expose any strings that were left
   hardcoded in the source, since they won't get transformed.


 - UI space not adapting to longer text.

   Many languages use longer strings than English. For example, German strings
   may be 30% longer (or more). Turning on pseudolocalization is a quick way to
   test how the layout handles such locales.


 - Bidi adaptation.

   For many developers, testing the UI in right-to-left mode is hard.
   Pseudolocalization shows how a right-to-left locale will look like.

To turn on pseudolocalization, add a new string pref :js:`intl.l10n.pseudo` and
select the strategy to be used:

 - :js:`accented` - Ȧȧƈƈḗḗƞŧḗḗḓ Ḗḗƞɠŀīīşħ

   This strategy replaces all Latin characters with their accented equivalents,
   and duplicates some vowels to create roughly 30% longer strings.


 - :js:`bidi` - ɥsıʅƃuƎ ıpıԐ

   This strategy replaces all Latin characters with their 180 degree rotated versions
   and enforces right to left text flow using Unicode UAX#9 `Explicit Directional Embeddings`__.
   In this mode, the UI directionality will also be set to right-to-left.

__ https://www.unicode.org/reports/tr9/#Explicit_Directional_Embeddings

Inner Structure of Fluent
=========================

The inner structure of Fluent in Gecko is out of scope of this tutorial, but
since the class and file names may show up during debugging or profiling,
below is a list of major components, each with a corresponding file in `/intl/l10n`
modules in Gecko.

FluentBundle
--------------

FluentBundle is the lowest level API. It's fully synchronous, contains a parser for the
FTL file format and a resolver for the logic. It is not meant to be used by
consumers directly.

In the future we intend to offer this layer for standardization and it may become
part of the :js:`mozIntl.*` or even :js:`Intl.*` API sets.

That part of the codebase is also the first that we'll be looking to port to Rust.


Localization
------------

Localization is a higher level API which uses :js:`FluentBundle` internally but
provides a full layer of compound message formatting and robust error fall-backing.

It is intended for use in runtime code and contains all fundamental localization
methods.


DOMLocalization
---------------

DOMLocalization extends :js:`Localization` with functionality to operate on HTML, XUL
and the DOM directly including DOM Overlays and Mutation Observers.

DocumentL10n
------------

DocumentL10n implements the DocumentL10n WebIDL API and allows Document to
communicate with DOMLocalization.

L10nRegistry
------------

L10nRegistry is our resource management service. It
maintains the state of resources packaged into the build and language packs,
providing an asynchronous iterator of :js:`FluentBundle` objects for a given locale set
and resources that the :js:`Localization` class uses.


.. _Fluent: https://projectfluent.org/
.. _DTD: https://developer.mozilla.org/en-US/docs/Mozilla/Tech/XUL/Tutorial/Localization
.. _StringBundle: https://developer.mozilla.org/en-US/docs/Mozilla/Tech/XUL/Tutorial/Property_Files
.. _Firefox Preferences: https://bugzilla.mozilla.org/show_bug.cgi?id=1415730
.. _Unprivileged Contexts: https://bugzilla.mozilla.org/show_bug.cgi?id=1407418
.. _System Add-ons: https://bugzilla.mozilla.org/show_bug.cgi?id=1425104
.. _CLDR: http://cldr.unicode.org/
.. _ICU: http://site.icu-project.org/
.. _Unicode: https://www.unicode.org/
.. _Fluent Syntax Guide: https://projectfluent.org/fluent/guide/
.. _Pontoon: https://pontoon.mozilla.org/
.. _Plural Rules: http://cldr.unicode.org/index/cldr-spec/plural-rules
