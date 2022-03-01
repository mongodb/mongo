/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

module.exports = {
  "plugins": [
    "spidermonkey-js"
  ],

  "overrides": [{
    "files": ["*.js"],
    "processor": "spidermonkey-js/processor",
  }],

  "rules": {
    // We should fix those at some point, but we use this to detect NaNs.
    "no-self-compare": "off",
    "no-lonely-if": "off",
    // Manually defining all the selfhosted methods is a slog.
    "no-undef": "off",
    // Disabled until we can use let/const to fix those erorrs,
    // and undefined names cause an exception and abort during runtime initialization.
    "no-redeclare": "off",
  }
};
