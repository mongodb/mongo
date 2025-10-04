/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

tests.set(
  "propertyTreeSplitting",
  (function() {
    var garbage = [];
    var garbageIndex = 0;
    var obj = {};
    return {
      description: "use delete to generate Shape garbage (piles are unused)",
      load: N => {},
      unload: () => {},
      makeGarbage: N => {
        for (var a = 0; a < N; ++a) {
          obj.x = 1;
          obj.y = 2;
          delete obj.x;
        }
      },
    };
  })()
);
