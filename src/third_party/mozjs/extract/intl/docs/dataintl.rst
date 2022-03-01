.. role:: js(code)
   :language: javascript

=========================
UI Internationalization
=========================

There are many types of data that need to be formatted into a locale specific format,
or require locale specific API operations.

Gecko provides a rich set of locale aware APIs for operations such as:

* date and time formatting
* number formatting
* searching
* sorting
* plural rules
* calendar and locale information

.. note::

  Most of the APIs are backed by the Unicode projects `CLDR`_ and `ICU`_ and are
  focused on enabling front-end code internationalization, which means the majority of
  the APIs are primarily available in JavaScript, with C++ and Rust having only a small
  subset of them exposed.

JavaScript Internationalization API
===================================

Data internationalization APIs are formalized in the JavaScript standard `ECMA 402`_.
These APIs are supported by all major JS environments.

It is best to consult the MDN article on the current state of the `Intl API`_.
Mozilla has an excellent support of the API and relies on it for majority
of its needs. Yet, when working on Firefox UI the :js:`Services.intl` wrapper
should be used.

Services.intl
=============

:js:`Services.intl` is an extension of the JS Intl API which should be used whenever
working with Gecko app user interface with chrome privileges.

The API provides the same objects and methods as :js:`Intl.*`, but fine tunes them
to the Gecko app user preferences, including matching OS Preferences and
other locale choices that web content exposed JS Intl API cannot.

For example, here's an example of a locale aware date formatting
using the regular :js:`Intl.DateTimeFormat`:

.. code-block:: javascript

    let rtf = new Intl.DateTimeFormat(navigator.languages, {
      year: "numeric",
      month: "long",
      day: "numeric"
    });
    let value = rtf.format(new Date());

It will do a good job at formatting the date to the user locale, but it will
only be able to use the customization bits that are exposed to the Web, based on
the locale the user broadcasts to the Web and any additional settings.

But that ignores bits of information that could inform the formatting.

Public API such as :js:`Intl.*` will not be able to look into the Operating System for
regional preferences. It will also respect settings such as `Resist Fingerprinting`
by masking its timezone and locale settings.

This is a fair tradeoff when dealing with the Web Content, but in most cases, the
privileged UI of the Gecko application should be able to access all of those
additional bits and not be affected by the anti-fingerprinting masking.

`mozIntl` is a simple wrapper which in its simplest form works exactly the same. It's
exposed on :js:`Services.intl` object and can be used just like a regular `Intl` API:

.. code-block:: javascript

    let rtf = new Services.intl.DateTimeFormat(undefined, {
      year: "numeric",
      month: "long",
      day: "numeric"
    });
    let value = rtf.format(new Date());

The difference is that this API will now use the set of locales as defined for
Gecko, and will also respect additional regional preferences that Gecko
will fetch from the Operating System.

For those reasons, when dealing with Gecko application UI, it is always recommended
to use the :js:`Services.intl` wrapper.

Additional APIs
================

On top of wrapping up `Intl` API, `mozIntl` provides a number of features
in form of additional options to existing APIs as well as completely new APIs.

Many of those extensions are in the process of being standardized, but are
already available to Gecko developers for internal use.

Below is the list of current extensions:

mozIntl.DateTimeFormat
----------------------

`DateTimeFormat` in `mozIntl` gets additional options that provide greater
simplicity and consistency to the API.

* :js:`timeStyle` and :js:`dateStyle` can take values :js:`short`, :js:`medium`,
  :js:`long` and :js:`full`.
  These options can replace the manual listing of tokens like :js:`year`, :js:`day`, :js:`hour` etc.
  and will compose the most natural date or time format of a given style for the selected
  locale.

Using :js:`timeStyle` and :js:`dateStyle` is highly recommended over listing the tokens,
because different locales may use different default styles for displaying the same tokens.

Additional value is that using those styles allows `mozIntl` to look into
Operating System patterns, which gives users the ability to customize those
patterns to their liking.

Example use:

.. code-block:: javascript

    let dtf = new Services.intl.DateTimeFormat(undefined, {
      timeStyle: "short",
      dateStyle: "short"
    });
    let value = dtf.format(new Date());

This will select the best locale to match the current Gecko application locale,
then potentially check for Operating System regional preferences customizations,
produce the correct pattern for short date+time style and format the date into it.


mozIntl.getCalendarInfo(locale)
-------------------------------

The API will return the following calendar information for a given locale code:

* firstDayOfWeek
    an integer in the range 1=Sunday to 7=Saturday indicating the day
    considered the first day of the week in calendars, e.g. 1 for en-US,
    2 for en-GB, 1 for bn-IN
* minDays
    an integer in the range of 1 to 7 indicating the minimum number
    of days required in the first week of the year, e.g. 1 for en-US, 4 for de
* weekendStart
    an integer in the range 1=Sunday to 7=Saturday indicating the day
    considered the beginning of a weekend, e.g. 7 for en-US, 7 for en-GB,
    1 for bn-IN
* weekendEnd
    an integer in the range 1=Sunday to 7=Saturday indicating the day
    considered the end of a weekend, e.g. 1 for en-US, 1 for en-GB,
    1 for bn-IN (note that "weekend" is *not* necessarily two days)

Those bits of information should be especially useful for any UI that works
with calendar data.

Example:

.. code-block:: javascript

    // omitting the `locale` argument will make the API return data for the
    // current Gecko application UI locale.
    let {
      firstDayOfWeek,  // 2
      minDays,         // 4
      weekendStart,    // 7
      weekendEnd,      // 1
      calendar,        // "gregory"
      locale,          // "pl"
    } = Services.intl.getCalendarInfo();


mozIntl.getDisplayNames(locales, options)
-----------------------------------------

:js:`getDisplayNames` API is useful to retrieve various terms available in the
internationalization API.

The API takes a locale fallback chain list, and an options object which can contain
two keys:

* :js:`style` which can takes values :js:`short`, :js:`medium`, :js:`long`
* :js:`keys` which is a list of keys in the following pattern:

  * :js:`dates/fields/{year|month|week|day}`
  * :js:`dates/gregorian/months/{january|...|december}`
  * :js:`dates/gregorian/weekdays/{sunday|...|saturday}`
  * :js:`dates/gregorian/dayperiods/{am|pm}`

The return object provides values for the requested keys for the given locale and
style.

Example:

.. code-block:: javascript

    let {
      locale,    // "pl"
      style,     // "long"
      values
    } = Services.intl.getDisplayNames(undefined, {
      style: "long",
      keys: [
        "dates/fields/year",
        "dates/gregorian/months/january",
        "dates/gregorian/weekdays/monday",
        "dates/gregorian/dayperiods/am"
      ]
    });

    values["dates/fields/year"] == "rok";
    values["dates/gregorian/months/january"] = "styczeń";
    values["dates/gregorian/weekdays/monday"] = "poniedziałek";
    values["dates/gregorian/dayperiods/am"] = "AM";


mozIntl.getLocaleInfo(locales, options)
---------------------------------------

The API returns a simple object with information about the requested locale.

At the moment the only bit handled by the API is directionality defined as `direction`
key on the returned object.

Example:

.. code-block:: javascript

    let {
      locale,    // "pl"
      direction: // "ltr"
    } = Services.intl.getLocaleInfo(undefined);


mozIntl.RelativeTimeFormat(locales, options)
--------------------------------------------

API which can be used to format an interval or a date into a textual
representation of a relative time, such as **5 minutes ago** or **in 2 days**.

This API is in the process of standardization and in its raw form will not handle
any calculations to select the best unit. It is intended to just offer a way
to format a value.

`mozIntl` wrapper extends the functionality providing the calculations and
allowing the user to get the current best textual representation of the delta.

Example:

.. code-block:: javascript

    let rtf = new Services.intl.RelativeTimeFormat(undefined, {
      style: "long", // "narrow" | "short" | "long" (default)
      numeric: "auto", // "always" | "auto" (default)
    });

    let now = Date.now();
    rtf.formatBestUnit(new Date(now - 3 * 1000 * 60)); // "3 minutes ago"

The option `numeric` has value set to `auto` by default, which means that when possible
the formatter will use special textual terms like *yesterday*, *last year*, and so on.

Those values require specific calculations that the raw `Intl.*` API cannot provide.
For example, *yesterday* requires the algorithm to know not only the time delta,
but also what time of the day `now` is. 15 hours ago may be *yesterday* if it
is 10am, but will still be *today* if it is 11pm.

For that reason the future `Intl.RelativeTimeFormat` will use *always* as default,
since terms such as *15 hours ago* are independent of the current time.

.. note::

  In the current form, the API should be only used to format standalone values.
  Without additional capitalization rules, it cannot be freely used in sentences.

mozIntl.getLanguageDisplayNames(locales, langCodes)
---------------------------------------------------

API which returns a list of language names formatted for display.

Example:

.. code-block:: javascript

  let langs = getLanguageDisplayNames(["pl"], ["fr", "de", "en"]);
  langs === ["Francuski", "Niemiecki", "Angielski"];


mozIntl.getRegionDisplayNames(locales, regionCodes)
---------------------------------------------------

API which returns a list of region names formatted for display.

Example:

.. code-block:: javascript

  let regs = getLanguageDisplayNames(["pl"], ["US", "CA", "MX"]);
  regs === ["Stany Zjednoczone", "Kanada", "Meksyk"];

mozIntl.getLocaleDisplayNames(locales, localeCodes)
---------------------------------------------------

API which returns a list of region names formatted for display.

Example:

.. code-block:: javascript

  let locs = getLanguageDisplayNames(["pl"], ["sr-RU", "es-MX", "fr-CA"]);
  locs === ["Serbski (Rosja)", "Hiszpański (Meksyk)", "Francuski (Kanada)"];

mozIntl.getAvailableLocaleDisplayNames(type)
---------------------------------------------------

API which returns a list of locale display name codes available for a
given type.
Available types are: "language", "region".

Example:

.. code-block:: javascript

  let codes = getAvailableLocaleDisplayNames("region");
  codes === ["au", "ae", "af", ...];

Best Practices
==============

The most important best practice when dealing with data internationalization is to
perform it as close to the actual UI as possible; right before the UI is displayed.

The reason for this practice is that internationalized data is considered *"opaque"*,
which means that no code should ever attempt to operate on it. Late resolution also
increases the chance that the data will be formatted in the current locale
selection and not formatted and cached prematurely.

It's very important to not attempt to search, concatenate or in any other way
alter the output of the API. Once it gets formatted, the only thing to do with
the output should be to present it to the user.

Testing
-------

The above is also important in the context of testing. It is a common mistake to
attempt to write tests that verify the output of the UI with internationalized data.

The underlying data set used to create the formatted version of the data may and will
change over time, both due to dataset improvements and also changes to the language
and regional preferences over time.
That means that tests that attempt to verify the exact output will require
significantly higher level of maintenance and will remain brittle.

Most of the APIs provide special method, like :js:`resolvedOptions` which should be used
instead to verify that the output is matching the expectations.

Future extensions
=================

If you find yourself in the need of additional internationalization APIs not currently
supported, you can verify if the API proposal is already in the works here,
and file a bug in the component `Core::Internationalization`_ to request it.

.. _ECMA 402: https://tc39.github.io/ecma402/
.. _Intl API: https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/Intl
.. _CLDR: http://cldr.unicode.org/
.. _ICU: http://site.icu-project.org/
.. _Core::Internationalization: https://bugzilla.mozilla.org/enter_bug.cgi?product=Core&component=Internationalization
