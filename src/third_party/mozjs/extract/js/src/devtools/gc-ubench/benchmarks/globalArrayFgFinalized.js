/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

tests.set(
  "globalArrayFgFinalized",
  (function() {
    var garbage = [];
    var garbageIndex = 0;
    return {
      description:
        "var foo = [ new Map, new Map, ... ]; # (foreground finalized)",

      load: N => {
        garbage = new Array(N);
      },
      unload: () => {
        garbage = [];
        garbageIndex = 0;
      },

      defaultGarbagePiles: "8K",
      defaultGarbagePerFrame: "48K",

      makeGarbage: N => {
        var arr = [];
        for (var i = 0; i < N; i++) {
          arr.push(new Map());
        }
        garbage[garbageIndex++] = arr;
        if (garbageIndex == garbage.length) {
          garbageIndex = 0;
        }
      },
    };
  })()
);
