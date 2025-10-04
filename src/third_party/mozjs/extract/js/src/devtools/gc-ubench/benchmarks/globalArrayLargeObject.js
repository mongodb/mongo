/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

tests.set(
  "globalArrayLargeObject",
  (function() {
    var garbage = [];
    var garbageIndex = 0;
    return {
      description: "var foo = { LARGE }; # (large slots)",

      load: N => {
        garbage = new Array(N);
      },
      unload: () => {
        garbage = [];
        garbageIndex = 0;
      },

      defaultGarbagePiles: "8K",
      defaultGarbagePerFrame: "64K",

      makeGarbage: N => {
        var obj = {};
        for (var i = 0; i < N; i++) {
          obj["key" + i] = i;
        }
        garbage[garbageIndex++] = obj;
        if (garbageIndex == garbage.length) {
          garbageIndex = 0;
        }
      },
    };
  })()
);
