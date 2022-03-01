/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

tests.set(
  "globalArrayBuffer",
  (function() {
    var garbage = [];
    var garbageIndex = 0;
    return {
      description: "var foo = ArrayBuffer(N); # (large malloc data)",

      load: N => {
        garbage = new Array(N);
      },
      unload: () => {
        garbage = [];
        garbageIndex = 0;
      },

      defaultGarbagePerFrame: "4M",
      defaultGarbagePiles: "8K",

      makeGarbage: N => {
        var ab = new ArrayBuffer(N);
        var view = new Uint8Array(ab);
        view[0] = 1;
        view[N - 1] = 2;
        garbage[garbageIndex++] = ab;
        if (garbageIndex == garbage.length) {
          garbageIndex = 0;
        }
      },
    };
  })()
);
