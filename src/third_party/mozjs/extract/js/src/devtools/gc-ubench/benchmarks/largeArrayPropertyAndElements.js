/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

tests.set(
  "largeArrayPropertyAndElements",
  (function() {
    var garbage;
    var index;

    return {
      description: "Large array with both properties and elements",

      load: n => {
        garbage = new Array(n);
        garbage.fill(null);
        index = 0;
      },

      unload: () => {
        garbage = null;
        index = 0;
      },

      defaultGarbagePiles: "100K",
      defaultGarbagePerFrame: "48K",

      makeGarbage: n => {
        for (var i = 0; i < n; i++) {
          index++;
          index %= garbage.length;

          var obj = {};
          garbage[index] = obj;
          garbage["key-" + index] = obj;
        }
      },
    };
  })()
);
