/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * This unit test makes sure that PluralForm.get can be called from strict mode
 */

const { PluralForm } = ChromeUtils.import(
  "resource://gre/modules/PluralForm.jsm"
);

delete PluralForm.numForms;
delete PluralForm.get;
[PluralForm.get, PluralForm.numForms] = PluralForm.makeGetter(9);

function run_test() {
  "use strict";

  Assert.equal(3, PluralForm.numForms());
  Assert.equal("one", PluralForm.get(5, "one;many"));
}
