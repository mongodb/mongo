Startup
=======

There are cases where it may be important to understand how Gecko locale management
acts during the startup.

Below is the description of the `server` mode, since the `client` mode is starting
with no data and doesn't perform any operations waiting for the parent to fill
basic locale lists (`requested` and `appLocales`) and then maintain them in a
unidirectional way.

Data Types
----------

There are two primary data types involved in negotiating locales during startup:
`requested` and `available`.
Throughout the startup different sources for this lists become available, and
in result the values for those lists change.

Data Sources
------------

There are three primary sources that become available during the bootstrap.

1) Packaged locale lists stored in files :js:`update.locale` and :js:`multilocale.txt`.

2) User preferences read from the profile.

3) Language packs installed in user profile or system wide.

Bootstrap
---------

1) Packaged Data
^^^^^^^^^^^^^^^^

In the `server` mode Gecko starts with no knowledge of `available` or `requested`
locales.

Initially, all fields are resolved lazily, so no data for available, requested,
default or resolved locales is retrieved.

If any code queries any of the APIs, it triggers the initial data fetching
and language negotiation.

The initial request comes from the XPCLocale which is initializing
the first JS context and needs to know which locale the JS context should use as
the default.

At that moment :js:`LocaleService` fetches the list of available locales, using
packaged locales which are retrieved via :js:`multilocale.txt` file in the toolkit's
package.
This gives LocaleService information about which locales are initially available.

Notice that this happens before any of the language packs gets registered, so
at that point Gecko only knows about packaged locales.

For requested locales, the initial request comes before user profile preferences
are being read, so the data is being fetched using packaged preferences.

In case of Desktop Firefox the :js:`intl.locale.requested` pref will be not set,
which means Gecko will use the default locale which is retrieved from
:js:`update.locale` file (also packaged).

This means that the initial result of language negotiation is between packaged
locales as available and the default requested locale.

2) Profile Prefs Read
^^^^^^^^^^^^^^^^^^^^^

Next, the profile is being read and if the user set any requested locales,
LocaleService updates its list of requested locales and broadcasts
:js:`intl:requested-locales-changed` event.

This may lead to language renegotiation if the requested locale is one of the packaged
ones. In that case, :js:`intl:app-locales-changed` will be broadcasted.

3) Language Packs Registered
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Finally, the AddonManager registers all the language packs, they get added to
:js:`L10nRegistry` and in result LocaleService's available locales get updated.

That triggers language negotiation and, if the language from the language pack
is used in the requested list, final list of locales is being set.

All of that happens before any UI is being built, but there's no guarantee of this
order being preserved, so it is important to understand that, depending on where the
code is used during the startup, it may receive different list of locales.

In order to maintain the correct locale settings it is important to set an observer
on :js:`intl:app-locales-changed` and update the code when the locale list changes.

That ensures the code always uses the best possible locale selection during startup,
but also during runtime in case user changes their requested locale list, or
language packs are updated/removed on the fly.
