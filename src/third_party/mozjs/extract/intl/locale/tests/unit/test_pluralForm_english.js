/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * This unit test makes sure the plural form for the default language (by
 * development), English, is working for the PluralForm javascript module.
 */

const { PluralForm } = ChromeUtils.import(
  "resource://gre/modules/PluralForm.jsm"
);

function run_test() {
  // English has 2 plural forms
  Assert.equal(2, PluralForm.numForms());

  // Make sure for good inputs, things work as expected
  for (var num = 0; num <= 200; num++) {
    Assert.equal(
      num == 1 ? "word" : "words",
      PluralForm.get(num, "word;words")
    );
  }

  // Not having enough plural forms defaults to the first form
  Assert.equal("word", PluralForm.get(2, "word"));

  // Empty forms defaults to the first form
  Assert.equal("word", PluralForm.get(2, "word;"));
}
