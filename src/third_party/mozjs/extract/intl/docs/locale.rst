.. role:: js(code)
   :language: javascript

=================
Locale management
=================

A locale is a combination of language, region, script, and regional preferences the
user wants to format their data into.

There are multiple models of locale data structures in the industry that have varying degrees
of compatibility between each other. Historically, each major platform has used their own,
and many standard bodies provided conflicting proposals.

Mozilla, alongside with most modern platforms, follows Unicode and W3C recommendation
and conforms to a standard known as `BCP 47`_ which describes a low level textual
representation of a locale known as `language tag`.

A few examples of language tags: *en-US*, *de*, *ar*, *zh-Hans*, *es-CL*.

Locales and Language Tags
=========================

Locale data structure consists of four primary fields.

 - Language (Example: English - *en*, French - *fr*, Serbian - *sr*)
 - Script (Example: Latin - *Latn*, Cyrylic - *Cyrl*)
 - Region (Example: United States - *US*, Canada - *CA*, Russia - *RU*)
 - Variants (Example: Mac OS - *macos*, Windows - *windows*, Linux - *linux*)

`BCP 47`_ specifies the syntax for each of those fields (called subtags) when
represented as a string. The syntax defines the allowed selection of characters,
their capitalization, and the order in which the fields should be defined.

Most of the base subtags are valid ISO codes, such as `ISO 639`_ for
language subtag, or `ISO 3166-1`_ for region.

The examples above present language tags with several fields omitted, which is allowed
by the standard.

On top of that, a locale may contain:

 - extensions and private fields
     These fields can be used to carry additional information about a locale.
     Mozilla currently has partial support for them in the JS implementation and plans to
     extend support to all APIs.
 - extkeys and "grandfathered" tags (unfortunate language, but part of the spec)
     Mozilla does not support these yet.


An example locale can be visualized as:

.. code-block:: javascript

  {
      "language": "sr",
      "script": "Cyrl",
      "region": "RU",
      "variants": [],
      "extensions": {},
      "privateuse": [],
  }

which can be then serialized into a string: **"sr-Cyrl-RU"**.

.. important::

  Since locales are often stored and passed around the codebase as
  language tag strings, it is important to always use an appropriate
  API to parse, manipulate and serialize them.
  Avoid `Do-It-Yourself` solutions which leave your code fragile and may
  break on unexpected language tag structures.

Locale Fallback Chains
======================

Locale sensitive operations are always considered "best-effort". That means that it
cannot be assumed that a perfect match will exist between what the user requested and what
the API can provide.

As a result, the best practice is to *always* operate on locale fallback chains -
ordered lists of locales according to the user preference.

An example of a locale fallback chain may be: :js:`["es-CL", "es-ES", "es", "fr", "en"]`.

The above means a request to format the data according to the Chilean Spanish if possible,
fall back to Spanish Spanish, then any (generic) Spanish, French and eventually to
English.

.. important::

  It is *always* better to use a locale fallback chain over a single locale.
  In case there's only one locale available, a list with one element will work
  while allowing for future extensions without a costly refactor.

Language Negotiation
====================

Due to the imperfections in data matching, all operations on locales should always
use a language negotiation algorithm to resolve the best available set of locales,
based on the list of all available locales and an ordered list of requested locales.

Such algorithms may vary in sophistication and number of strategies. Mozilla's
solution is based on modified logic from `RFC 5656`_.

The three lists of locales used in negotiation:

 - **Available** - locales that are locally installed
 - **Requested** - locales that the user selected in decreasing order of preference
 - **Resolved** - result of the negotiation

The result of a negotiation is an ordered list of locales that are available to
the system, and the consumer is expected to attempt using the locales in the
resolved order.

Negotiation should be used in all scenarios like selecting language resources,
calendar, number formatting, etc.

Single Locale Matching
----------------------

Every negotiation strategy goes through a list of steps in an attempt to find the
best possible match between locales.

The exact algorithm is custom, and consists of a 6 level strategy:

::

  1) Attempt to find an exact match for each requested locale in available
     locales.
     Example: ['en-US'] * ['en-US'] = ['en-US']

  2) Attempt to match a requested locale to an available locale treated
     as a locale range.
     Example: ['en-US'] * ['en'] = ['en']
                            ^^
                            |-- becomes 'en-*-*-*'

  3) Attempt to use the maximized version of the requested locale, to
     find the best match in available locales.
     Example: ['en'] * ['en-GB', 'en-US'] = ['en-US']
                ^^
                |-- ICU likelySubtags expands it to 'en-Latn-US'

  4) Attempt to look for a different variant of the same locale.
     Example: ['ja-JP-win'] * ['ja-JP-mac'] = ['ja-JP-mac']
                ^^^^^^^^^
                |----------- replace variant with range: 'ja-JP-*'

  5) Attempt to look for a maximized version of the requested locale,
     stripped of the region code.
     Example: ['en-CA'] * ['en-ZA', 'en-US'] = ['en-US', 'en-ZA']
                ^^^^^
                |----------- look for likelySubtag of 'en': 'en-Latn-US'

  6) Attempt to look for a different region of the same locale.
     Example: ['en-GB'] * ['en-AU'] = ['en-AU']
                ^^^^^
                |----- replace region with range: 'en-*'

Filtering / Matching / Lookup
-----------------------------

When negotiating between lists of locales, Mozilla's :js:`LocaleService` API
offers three language negotiation strategies:

Filtering
^^^^^^^^^

This is the most common scenario, where there is an advantage in creating a
maximal possible list of locales that the user may benefit from.

An example of a scenario:

.. code-block:: javascript

    let requested = ["fr-CA", "en-US"];
    let available = ["en-GB", "it", "en-ZA", "fr", "de-DE", "fr-CA", "fr-CH"];

    let result = Services.locale.negotiateLanguages(requested, available);

    result == ["fr-CA", "fr", "fr-CH", "en-GB", "en-ZA"];

In the example above the algorithm was able to match *"fr-CA"* as a perfect match,
but then was able to find other matches as well - a generic French is a very
good match, and Swiss French is also very close to the top requested language.

In case of the second of the requested locales, unfortunately American English
is not available, but British English and South African English are.

The algorithm is greedy and attempts to match as many locales
as possible. This is usually what the developer wants.

Matching
^^^^^^^^

In less common scenarios the code needs to match a single, best available locale for
each of the requested locales.

An example of this scenario:

.. code-block:: javascript

    let requested = ["fr-CA", "en-US"];
    let available = ["en-GB", "it", "en-ZA", "fr", "de-DE", "fr-CA", "fr-ZH"];

    let result = Services.locale.negotiateLanguages(
      requested,
      available,
      undefined,
      Services.locale.langNegStrategyMatching);

    result == ["fr-CA", "en-GB"];

The best available locales for *"fr-CA"* is a perfect match, and for *"en-US"*, the
algorithm selected British English.

Lookup
^^^^^^

The third strategy should be used in cases where no matter what, only one locale
can be ever used. Some third-party APIs don't support fallback and it doesn't make
sense to continue resolving after finding the first locale.

It is still advised to continue using this API as a fallback chain list, just in
this case with a single element.

.. code-block:: javascript

    let requested = ["fr-CA", "en-US"];
    let available = ["en-GB", "it", "en-ZA", "fr", "de-DE", "fr-CA", "fr-ZH"];

    let result = Services.locale.negotiateLanguages(
      requested,
      available,
      Services.locale.defaultLocale,
      Services.locale.langNegStrategyLookup);

    result == ["fr-CA"];

Default Locale
--------------

Besides *Available*, *Requested* and *Resolved* locale lists, there's also a concept
of *DefaultLocale*, which is a single locale out of the list of available ones that
should be used in case there is no match to be found between available and
requested locales.

Every Firefox is built with a single default locale - for example
**Firefox zh-CN** has *DefaultLocale* set to *zh-CN* since this locale is guaranteed
to be packaged in, have all the resources, and should be used if the negotiation fails
to return any matches.

.. code-block:: javascript

    let requested = ["fr-CA", "en-US"];
    let available = ["it", "de", "zh-CN", "pl", "sr-RU"];
    let defaultLocale = "zh-CN";

    let result = Services.locale.negotiateLanguages(requested, available, defaultLocale);

    result == ["zh-CN"];

Chained Language Negotiation
----------------------------

In some cases the user may want to link a language selection to another component.

For example, a Firefox extension may come with its own list of available locales, which
may have locales that Firefox doesn't.

In that case, negotiation between user requested locales and the add-on's list may result
in a selection of locales superseding that of Firefox itself.


.. code-block:: none

         Fx Available
        +-------------+
        |  it, fr, ar |
        +-------------+                 Fx Locales
                      |                +--------+
                      +--------------> | fr, ar |
                      |                +--------+
            Requested |
     +----------------+
     | es, fr, pl, ar |
     +----------------+                 Add-on Locales
                      |                +------------+
                      +--------------> | es, fr, ar |
      Add-on Available |               +------------+
    +-----------------+
    |  de, es, fr, ar |
    +-----------------+


In that case, an add-on may end up being displayed in Spanish, while Firefox UI will
use French. In most cases this results in a bad UX.

In order to avoid that, one can chain the add-on negotiation and take Firefox's resolved
locales as a `requested`, and negotiate that against the add-ons' `available` list.

.. code-block:: none

        Fx Available
       +-------------+
       |  it, ar, fr |
       +-------------+                Fx Locales (as Add-on Requested)
                     |                +--------+
                     +--------------> | fr, ar |
                     |                +--------+
           Requested |                         |                Add-on Locales
    +----------------+                         |                +--------+
    | es, fr, pl, ar |                         +------------->  | fr, ar |
    +----------------+                         |                +--------+
                                               |
                              Add-on Available |
                             +-----------------+
                             |  de, es, ar, fr |
                             +-----------------+

Available Locales
=================

In Gecko, available locales come from the `Packaged Locales` and the installed
`language packs`. Language packs are a variant of web extensions providing just
localized resources for one or more languages.

The primary notion of which locales are available is based on which locales Gecko has
UI localization resources for, and other datasets such as internationalization may
carry different lists of available locales.

Requested Locales
=================

The list of requested locales can be read and set using :js:`LocaleService::requestedLocales` API.

Using the API will perform necessary sanity checks and canonicalize the values.

After the sanitization, the value will be stored in a pref :js:`intl.locale.requested`.
The pref usually will store a comma separated list of valid BCP47 locale
codes, but it can also have two special meanings:

 - If the pref is not set at all, Gecko will use the default locale as the requested one.
 - If the pref is set to an empty string, Gecko will look into OS app locales as the requested.

The former is the current default setting for Firefox Desktop, and the latter is the
default setting for Firefox for Android.

If the developer wants to programmatically request the app to follow OS locales,
they can assign :js:`null` to :js:`requestedLocales`.

Regional Preferences
====================

Every locale comes with a set of default preferences that are specific to a culture
and region. This contains preferences such as calendar system, way to display
time (24h vs 12h clock), which day the week starts on, which days constitute a weekend,
what numbering system and date time formatting a given locale uses
(for example "MM/DD" in en-US vs "DD/MM" in en-AU).

For all such preferences Gecko has a list of default settings for every region,
but there's also a degree of customization every user may want to make.

All major operating systems have a Settings UI for selecting those preferences,
and since Firefox does not provide its own, Gecko looks into the OS for them.

A special API :js:`mozilla::intl::OSPreferences` handles communication with the
host operating system, retrieving regional preferences and altering
internationalization formatting with user preferences.

One thing to notice is that the boundary between regional preferences and language
selection is not strong. In many cases the internationalization formats
will contain language specific terms and literals. For example a date formatting
pattern into Japanese may look like this - *"2018年3月24日"*, or the date format
may contains names of months or weekdays to be translated
("April", "Tuesday" etc.).

For that reason it is tricky to follow regional preferences in a scenario where Operating
System locale selection does not match the Firefox UI locales.

Such behavior might lead to a UI case like "Today is 24 października" in an English Firefox
with Polish date formats.

For that reason, by default, Gecko will *only* look into OS Preferences if the *language*
portion of the locale of the OS and Firefox match.
That means that if Windows is in "**en**-AU" and Firefox is in "**en**-US" Gecko will look
into Windows Regional Preferences, but if Windows is in "**de**-CH" and Firefox
is in "**fr**-FR" it won't.
In order to force Gecko to look into OS preferences irrelevant of the language match,
set the flag :js:`intl.regional_prefs.use_os_locales` to :js:`true`.

UI Direction
------------

Since the UI direction is so tightly coupled with the locale selection, the
main method of testing the directionality of the Gecko app lives in LocaleService.

:js:`LocaleService::IsAppLocaleRTL` returns a boolean indicating if the current
direction of the app UI is right-to-left.

Default and Last Fallback Locales
=================================

Every Gecko application is built with a single locale as the default one. Such locale
is guaranteed to have all linguistic resources available, should be used
as the default locale in case language negotiation cannot find any match, and also
as the last locale to look for in a fallback chain.

If all else fails, Gecko also support a notion of last fallback locale, which is
currently hardcoded to *"en-US"*, and is the very final locale to try in case
nothing else (including the default locale) works.
Notice that Unicode and ICU use *"en-GB"* in that role because more English speaking
people around the World recognize British regional preferences than American (metric vs.
imperial, Fahrenheit vs Celsius etc.).
Mozilla may switch to *"en-GB"* in the future.

Packaged Locales
================

When the Gecko application is being packaged it bundles a selection of locale resources
to be available within it. At the moment, for example, most Firefox for Android
builds come with almost 100 locales packaged into it, while Desktop Firefox comes
with usually just one packaged locale.

There is currently work being done on enabling more flexibility in how
the locales are packaged to allow for bundling applications with different
sets of locales in different areas - dictionaries, hyphenations, product language resources,
installer language resources, etc.

Web Exposed Locales
====================

For anti-tracking or some other reasons, we tend to expose spoofed locale to web content instead
of default locales. This can be done by setting the pref :js:`intl.locale.privacy.web_exposed`.
The pref is a comma separated list of locale, and empty string implies default locales.

The pref has no function while :js:`privacy.spoof_english` is set to 2, where *"en-US"* will always
be returned.

Multi-Process
=============

Locale management can operate in a client/server model. This allows a Gecko process
to manage locales (server mode) or just receive the locale selection from a parent
process (client mode).

The client mode is currently used by all child processes of Desktop Firefox, and
may be used by, for example, GeckoView to follow locale selection from a parent
process.

To check the mode the process is operating in, the :js:`LocaleService::IsServer` method is available.

Note that :js:`L10nRegistry.registerSources`, :js:`L10nRegistry.updateSources`, and
:js:`L10nRegistry.removeSources` each trigger an IPC synchronization between the parent
process and any extant content processes, which is expensive. If you need to change the
registration of multiple sources, the best way to do so is to coalesce multiple requests
into a single array and then call the method once.

Mozilla Exceptions
==================

There's currently only a single exception of the BCP47 used, and that's
a legacy "ja-JP-mac" locale. The "mac" is a variant and BCP47 requires all variants
to be 5-8 character long.

Gecko supports the limitation by accepting the 3-letter variants in our APIs and also
provides a special :js:`appLocalesAsLangTags` method which returns this locale in that form.
(:js:`appLocalesAsBCP47` will canonicalize it and turn into `"ja-JP-macos"`).

Usage of language negotiation etc. shouldn't rely on this behavior.

Events
======

:js:`LocaleService` emits two events: :js:`intl:app-locales-changed` and
:js:`intl:requested-locales-changed` which all code can listen to.

Those events may be broadcasted in response to new language packs being installed, or
uninstalled, or user selection of languages changing.

In most cases, the code should observe the :js:`intl:app-locales-changed`
and react to only that event since this is the one indicating a change
in the currently used language settings that the components should follow.

Testing
=======

Many components may have logic encoded to react to changes in requested, available
or resolved locales.

In order to test the component's behavior, it is important to replicate
the environment in which such change may happen.

Since in most cases it is advised for a component to tie its
language negotiation to the main application (see `Chained Language Negotiation`),
it is not enough to add a new locale to trigger the language change.

First, it is necessary to add a new locale to the available ones, then change
the requested, and only that will result in a new negotiation and language
change happening.

There are two primary ways to add a locale to available ones.

Testing Localization
--------------------

If the goal is to test that the correct localization ends up in the correct place,
the developer needs to register a new :js:`FileSource` in :js:`L10nRegistry` and
provide a mock cached data to be returned by the API.

It may look like this:

.. code-block:: javascript

    let fs = new FileSource(["ko-KR", "ar"], "resource://mock-addon/localization/{locale}");

    fs.cache = {
      "resource://mock-addon/localization/ko-KR/test.ftl": "key = Value in Korean",
      "resource://mock-addon/localization/ar/test.ftl": "key = Value in Arabic"
    };

    L10nRegistry.registerSources([fs]);

    let availableLocales = Services.locale.availableLocales;

    assert(availableLocales.includes("ko-KR"));
    assert(availableLocales.includes("ar"));

    Services.locale.requestedLocales = ["ko-KR"];

    let appLocales = Services.locale.appLocalesAsBCP47;
    assert(appLocales[0], "ko-KR");

From here, a resource :js:`test.ftl` can be added to a `Localization` and for ID :js:`key`
the correct value from the mocked cache will be returned.

Testing Locale Switching
------------------------

The second method is much more limited, as it only mocks the locale availability,
but it is also simpler:

.. code-block:: javascript

    Services.locale.availableLocales = ["ko-KR", "ar"];
    Services.locale.requestedLocales = ["ko-KR"];

    let appLocales = Services.locale.appLocalesAsBCP47;
    assert(appLocales[0], "ko-KR");

In the future, Mozilla plans to add a third way for add-ons (`bug 1440969`_)
to allow for either manual or automated testing purposes disconnecting its locales
from the main application ones.

Testing the outcome
-------------------

Except of testing for reaction to locale changes, it is advised to avoid writing
tests that expect a certain locale to be selected, or certain internationalization
or localization data to be used.

Doing so locks down the test infrastructure to be only usable when launched in
a single locale environment and requires those tests to be updated whenever the underlying
data changes.

In the case of testing locale selection it is best to use a fake locale like :js:`x-test`, that
will not be present at the beginning of the test.

In the case of testing for internationalization data it is best to use :js:`resolvedOptions()`,
to verify the right data is being used, rather than comparing the output string.

In the case of localization, it is best to test against the correct :js:`data-l10n-id`
being set or, in edge cases, verify that a given variable is present in the string using
:js:`String.prototype.includes`.

Deep Dive
=========

Below is a list of articles with additional
details on selected subjects:

.. toctree::
   :maxdepth: 1

   locale_env
   locale_startup

Feedback
========

In case of questions, please consult Intl module peers.


.. _RFC 5656: https://tools.ietf.org/html/rfc5656
.. _BCP 47: https://tools.ietf.org/html/bcp47#section-2.1
.. _ISO 639: http://www.loc.gov/standards/iso639-2/php/code_list.php
.. _ISO 3166-1: https://www.iso.org/iso-3166-country-codes.html
.. _Intl.Locale: https://bugzilla.mozilla.org/show_bug.cgi?id=1433303
.. _fluent-locale: https://docs.rs/fluent-locale/
.. _bug 1440969: https://bugzilla.mozilla.org/show_bug.cgi?id=1440969
