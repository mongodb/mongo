"use strict";

module.exports = {
  "plugins": [
    "spidermonkey-js"
  ],

  "rules": {
    // We should fix those at some point, but we use this to detect NaNs.
    "no-self-compare": "off",
    // Disabling these two make it easier to implement the spec.
    "spaced-comment": "off",
    "no-lonely-if": "off",
    // SpiderMonkey's style doesn't match any of the possible options.
    "brace-style": "off",
    // Manually defining all the selfhosted methods is a slog.
    "no-undef": "off",
    // Disabled until we can use let/const to fix those erorrs,
    // and undefined names cause an exception and abort during runtime initialization.
    "no-redeclare": "off",
  }
};
