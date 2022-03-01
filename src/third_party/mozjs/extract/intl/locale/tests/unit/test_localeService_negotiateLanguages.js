/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const { Services } = ChromeUtils.import("resource://gre/modules/Services.jsm");
const localeService = Services.locale;

const data = {
  filtering: {
    "exact match": [
      [["en"], ["en"], ["en"]],
      [["en-US"], ["en-US"], ["en-US"]],
      [["en-Latn-US"], ["en-Latn-US"], ["en-Latn-US"]],
      [["en-Latn-US-windows"], ["en-Latn-US-windows"], ["en-Latn-US-windows"]],
      [["fr-FR"], ["de", "it", "fr-FR"], ["fr-FR"]],
      [
        ["fr", "pl", "de-DE"],
        ["pl", "en-US", "de-DE"],
        ["pl", "de-DE"],
      ],
    ],
    "available as range": [
      [["en-US"], ["en"], ["en"]],
      [["en-Latn-US"], ["en-US"], ["en-US"]],
      [["en-US-windows"], ["en-US"], ["en-US"]],
      [
        ["fr-CA", "de-DE"],
        ["fr", "it", "de"],
        ["fr", "de"],
      ],
      [["ja-JP-windows"], ["ja"], ["ja"]],
      [
        ["en-Latn-GB", "en-Latn-IN"],
        ["en-IN", "en-GB"],
        ["en-GB", "en-IN"],
      ],
    ],
    "should match on likely subtag": [
      [["en"], ["en-GB", "de", "en-US"], ["en-US", "en-GB"]],
      [
        ["en"],
        ["en-Latn-GB", "de", "en-Latn-US"],
        ["en-Latn-US", "en-Latn-GB"],
      ],
      [["fr"], ["fr-CA", "fr-FR"], ["fr-FR", "fr-CA"]],
      [["az-IR"], ["az-Latn", "az-Arab"], ["az-Arab"]],
      [["sr-RU"], ["sr-Cyrl", "sr-Latn"], ["sr-Latn"]],
      [["sr"], ["sr-Latn", "sr-Cyrl"], ["sr-Cyrl"]],
      [["zh-GB"], ["zh-Hans", "zh-Hant"], ["zh-Hant"]],
      [["sr", "ru"], ["sr-Latn", "ru"], ["ru"]],
      [["sr-RU"], ["sr-Latn-RO", "sr-Cyrl"], ["sr-Latn-RO"]],
    ],
    "should match likelySubtag region over other regions": [
      [["en-CA"], ["en-ZA", "en-GB", "en-US"], ["en-US", "en-ZA", "en-GB"]],
    ],
    "should match cross-region": [
      [["en"], ["en-US"], ["en-US"]],
      [["en-US"], ["en-GB"], ["en-GB"]],
      [["en-Latn-US"], ["en-Latn-GB"], ["en-Latn-GB"]],
    ],
    "should match cross-variant": [
      [["en-US-linux"], ["en-US-windows"], ["en-US-windows"]],
    ],
    "should prioritize properly": [
      // exact match first
      [
        ["en-US"],
        ["en-US-windows", "en", "en-US"],
        ["en-US", "en", "en-US-windows"],
      ],
      // available as range second
      [["en-Latn-US"], ["en-GB", "en-US"], ["en-US", "en-GB"]],
      // likely subtags third
      [["en"], ["en-Cyrl-US", "en-Latn-US"], ["en-Latn-US"]],
      // variant range fourth
      [
        ["en-US-macos"],
        ["en-US-windows", "en-GB-macos"],
        ["en-US-windows", "en-GB-macos"],
      ],
      // regional range fifth
      [["en-US-macos"], ["en-GB-windows"], ["en-GB-windows"]],
      [["en-US"], ["en-GB", "en"], ["en", "en-GB"]],
      [
        ["fr-CA-macos", "de-DE"],
        ["de-DE", "fr-FR-windows"],
        ["fr-FR-windows", "de-DE"],
      ],
    ],
    "should handle default locale properly": [
      [["fr"], ["de", "it"], []],
      [["fr"], ["de", "it"], "en-US", ["en-US"]],
      [["fr"], ["de", "en-US"], "en-US", ["en-US"]],
      [
        ["fr", "de-DE"],
        ["de-DE", "fr-CA"],
        "en-US",
        ["fr-CA", "de-DE", "en-US"],
      ],
    ],
    "should handle all matches on the 1st higher than any on the 2nd": [
      [
        ["fr-CA-macos", "de-DE"],
        ["de-DE", "fr-FR-windows"],
        ["fr-FR-windows", "de-DE"],
      ],
    ],
    "should handle cases and underscores, returning the form given in the 'available' list": [
      [["fr_FR"], ["fr-FR"], ["fr-FR"]],
      [["fr_fr"], ["fr-FR"], ["fr-FR"]],
      [["fr_Fr"], ["fr-fR"], ["fr-fR"]],
      [["fr_lAtN_fr"], ["fr-Latn-FR"], ["fr-Latn-FR"]],
      [["fr_FR"], ["fr_FR"], ["fr_FR"]],
      [["fr-FR"], ["fr_FR"], ["fr_FR"]],
      [["fr_Cyrl_FR_macos"], ["fr_Cyrl_fr-macos"], ["fr_Cyrl_fr-macos"]],
    ],
    "should handle mozilla specific 3-letter variants": [
      [
        ["ja-JP-mac", "de-DE"],
        ["ja-JP-mac", "de-DE"],
        ["ja-JP-mac", "de-DE"],
      ],
    ],
    "should not crash on invalid input": [
      [["fą-FŻ"], ["ór_Fń"], []],
      [["2"], ["ąóżł"], []],
      [[[""]], ["fr-FR"], []],
    ],
    "should not match on invalid input": [[["en"], ["ťŮ"], []]],
  },
  matching: {
    "should match only one per requested": [
      [
        ["fr", "en"],
        ["en-US", "fr-FR", "en", "fr"],
        null,
        localeService.langNegStrategyMatching,
        ["fr", "en"],
      ],
    ],
  },
  lookup: {
    "should match only one": [
      [
        ["fr-FR", "en"],
        ["en-US", "fr-FR", "en", "fr"],
        "en-US",
        localeService.langNegStrategyLookup,
        ["fr-FR"],
      ],
    ],
  },
};

function run_test() {
  const nl = localeService.negotiateLanguages;

  const json = JSON.stringify;
  for (const strategy in data) {
    for (const groupName in data[strategy]) {
      const group = data[strategy][groupName];
      for (const test of group) {
        const requested = test[0];
        const available = test[1];
        const defaultLocale = test.length > 3 ? test[2] : undefined;
        const strategyInner = test.length > 4 ? test[3] : undefined;
        const supported = test[test.length - 1];

        const result = nl(test[0], test[1], defaultLocale, strategyInner);
        deepEqual(
          result,
          supported,
          `\nExpected ${json(requested)} * ${json(available)} = ${json(
            supported
          )}.\n`
        );
      }
    }
  }

  // Verify that we error out when requested or available is not an array.
  for (const [req, avail] in [
    [null, ["fr-FR"]],
    [undefined, ["fr-FR"]],
    [2, ["fr-FR"]],
    ["fr-FR", ["fr-FR"]],
    [["fr-FR"], null],
    [["fr-FR"], undefined],
    [["fr-FR"], 2],
    [["fr-FR"], "fr-FR"],
    [[null], []],
    [[undefined], []],
    [[undefined], [null]],
    [[undefined], [undefined]],
    [[null], [null]],
    [[[]], [[2]]],
  ]) {
    Assert.throws(
      () => {
        nl(req, avail);
      },
      err => err.result == Cr.NS_ERROR_XPC_CANT_CONVERT_PRIMITIVE_TO_ARRAY
    );
  }
}
