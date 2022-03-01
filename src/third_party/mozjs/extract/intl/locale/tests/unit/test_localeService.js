/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const { Services } = ChromeUtils.import("resource://gre/modules/Services.jsm");
const osPrefs = Cc["@mozilla.org/intl/ospreferences;1"].getService(
  Ci.mozIOSPreferences
);

const localeService = Services.locale;

/**
 * Make sure the locale service can be instantiated.
 */

add_test(function test_defaultLocale() {
  const defaultLocale = localeService.defaultLocale;
  Assert.ok(defaultLocale.length !== 0, "Default locale is not empty");
  run_next_test();
});

add_test(function test_lastFallbackLocale() {
  const lastFallbackLocale = localeService.lastFallbackLocale;
  Assert.ok(lastFallbackLocale === "en-US", "Last fallback locale is en-US");
  run_next_test();
});

add_test(function test_appLocalesAsLangTags() {
  const appLocale = localeService.appLocaleAsLangTag;
  Assert.ok(appLocale != "", "appLocale is non-empty");

  const appLocales = localeService.appLocalesAsLangTags;
  Assert.ok(Array.isArray(appLocales), "appLocales returns an array");

  Assert.ok(
    appLocale == appLocales[0],
    "appLocale matches first entry in appLocales"
  );

  const enUSLocales = appLocales.filter(loc => loc === "en-US");
  Assert.ok(enUSLocales.length == 1, "en-US is present exactly one time");

  run_next_test();
});

const PREF_REQUESTED_LOCALES = "intl.locale.requested";
const REQ_LOC_CHANGE_EVENT = "intl:requested-locales-changed";

add_test(function test_requestedLocales() {
  const requestedLocales = localeService.requestedLocales;
  Assert.ok(
    Array.isArray(requestedLocales),
    "requestedLocales returns an array"
  );

  run_next_test();
});

/**
 * In this test we verify that after we set an observer on the LocaleService
 * event for requested locales change, it will be fired when the
 * pref for matchOS is set to true.
 *
 * Then, we test that when the matchOS is set to true, we will retrieve
 * OS locale from requestedLocales.
 */
add_test(function test_requestedLocales_matchOS() {
  do_test_pending();

  Services.prefs.setCharPref(PREF_REQUESTED_LOCALES, "ar-IR");

  const observer = {
    observe(aSubject, aTopic, aData) {
      switch (aTopic) {
        case REQ_LOC_CHANGE_EVENT:
          const reqLocs = localeService.requestedLocales;
          Assert.ok(reqLocs[0] === osPrefs.systemLocale);
          Services.obs.removeObserver(observer, REQ_LOC_CHANGE_EVENT);
          do_test_finished();
      }
    },
  };

  Services.obs.addObserver(observer, REQ_LOC_CHANGE_EVENT);
  Services.prefs.setCharPref(PREF_REQUESTED_LOCALES, "");

  run_next_test();
});

/**
 * In this test we verify that after we set an observer on the LocaleService
 * event for requested locales change, it will be fired when the
 * pref for browser UI locale changes.
 */
add_test(function test_requestedLocales_onChange() {
  do_test_pending();

  Services.prefs.setCharPref(PREF_REQUESTED_LOCALES, "ar-IR");

  const observer = {
    observe(aSubject, aTopic, aData) {
      switch (aTopic) {
        case REQ_LOC_CHANGE_EVENT:
          const reqLocs = localeService.requestedLocales;
          Assert.ok(reqLocs[0] === "sr-RU");
          Services.obs.removeObserver(observer, REQ_LOC_CHANGE_EVENT);
          do_test_finished();
      }
    },
  };

  Services.obs.addObserver(observer, REQ_LOC_CHANGE_EVENT);
  Services.prefs.setCharPref(PREF_REQUESTED_LOCALES, "sr-RU");

  run_next_test();
});

add_test(function test_requestedLocale() {
  Services.prefs.setCharPref(PREF_REQUESTED_LOCALES, "tlh");

  let requestedLocale = localeService.requestedLocale;
  Assert.ok(
    requestedLocale === "tlh",
    "requestedLocale returns the right value"
  );

  Services.prefs.clearUserPref(PREF_REQUESTED_LOCALES);

  run_next_test();
});

add_test(function test_requestedLocales() {
  localeService.requestedLocales = ["de-AT", "de-DE", "de-CH"];

  let locales = localeService.requestedLocales;
  Assert.ok(locales[0] === "de-AT");
  Assert.ok(locales[1] === "de-DE");
  Assert.ok(locales[2] === "de-CH");

  run_next_test();
});

add_test(function test_isAppLocaleRTL() {
  Assert.ok(typeof localeService.isAppLocaleRTL === "boolean");

  run_next_test();
});

add_test(function test_isAppLocaleRTL_pseudo() {
  let avLocales = localeService.availableLocales;
  let reqLocales = localeService.requestedLocales;

  localeService.availableLocales = ["en-US"];
  localeService.requestedLocales = ["en-US"];
  Services.prefs.setCharPref("intl.l10n.pseudo", "");

  Assert.ok(localeService.isAppLocaleRTL === false);

  Services.prefs.setCharPref("intl.l10n.pseudo", "bidi");
  Assert.ok(localeService.isAppLocaleRTL === true);

  Services.prefs.setCharPref("intl.l10n.pseudo", "accented");
  Assert.ok(localeService.isAppLocaleRTL === false);

  // Clean up
  localeService.availableLocales = avLocales;
  localeService.requestedLocales = reqLocales;
  Services.prefs.clearUserPref("intl.l10n.pseudo");

  run_next_test();
});

add_test(function test_packagedLocales() {
  const locales = localeService.packagedLocales;
  Assert.ok(locales.length !== 0, "Packaged locales are empty");
  run_next_test();
});

add_test(function test_availableLocales() {
  const avLocales = localeService.availableLocales;
  localeService.availableLocales = ["und", "ar-IR"];

  let locales = localeService.availableLocales;
  Assert.ok(locales.length == 2);
  Assert.ok(locales[0] === "und");
  Assert.ok(locales[1] === "ar-IR");

  localeService.availableLocales = avLocales;

  run_next_test();
});

/**
 * This test verifies that all values coming from the pref are sanitized.
 */
add_test(function test_requestedLocales_sanitize() {
  Services.prefs.setStringPref(
    PREF_REQUESTED_LOCALES,
    "de,2,#$@#,pl,ąó,!a2,DE-at,,;"
  );

  let locales = localeService.requestedLocales;
  Assert.equal(locales[0], "de");
  Assert.equal(locales[1], "pl");
  Assert.equal(locales[2], "de-AT");
  Assert.equal(locales.length, 3);

  Services.prefs.clearUserPref(PREF_REQUESTED_LOCALES);

  run_next_test();
});

add_test(function test_handle_ja_JP_mac() {
  const bkpAvLocales = localeService.availableLocales;

  localeService.availableLocales = ["ja-JP-mac", "en-US"];

  localeService.requestedLocales = ["ja-JP-mac"];

  let reqLocales = localeService.requestedLocales;
  Assert.equal(reqLocales[0], "ja-JP-macos");

  let avLocales = localeService.availableLocales;
  Assert.equal(avLocales[0], "ja-JP-macos");

  let appLocales = localeService.appLocalesAsBCP47;
  Assert.equal(appLocales[0], "ja-JP-macos");

  let appLocalesAsLT = localeService.appLocalesAsLangTags;
  Assert.equal(appLocalesAsLT[0], "ja-JP-mac");

  Assert.equal(localeService.appLocaleAsLangTag, "ja-JP-mac");

  localeService.availableLocales = bkpAvLocales;

  run_next_test();
});

registerCleanupFunction(() => {
  Services.prefs.clearUserPref(PREF_REQUESTED_LOCALES);
});
