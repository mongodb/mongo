/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * This unit test makes sure the plural form for Irish Gaeilge is working by
 * using the makeGetter method instead of using the default language (by
 * development), English.
 */

const { PluralForm } = ChromeUtils.import(
  "resource://gre/modules/PluralForm.jsm"
);

function run_test() {
  // Irish is plural rule #11
  let [get, numForms] = PluralForm.makeGetter(11);

  // Irish has 5 plural forms
  Assert.equal(5, numForms());

  // I don't really know Irish, so I'll stick in some dummy text
  let words = "is 1;is 2;is 3-6;is 7-10;everything else";

  let test = function(text, low, high) {
    for (let num = low; num <= high; num++) {
      Assert.equal(text, get(num, words));
    }
  };

  // Make sure for good inputs, things work as expected
  test("everything else", 0, 0);
  test("is 1", 1, 1);
  test("is 2", 2, 2);
  test("is 3-6", 3, 6);
  test("is 7-10", 7, 10);
  test("everything else", 11, 200);
}
