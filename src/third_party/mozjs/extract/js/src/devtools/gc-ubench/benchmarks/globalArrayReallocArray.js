/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

tests.set(
  "globalArrayReallocArray",
  (function() {
    var garbage = [];
    var garbageIndex = 0;
    return {
      description: "var foo = [[,,,], ....]",
      defaultGarbagePerFrame: "2M",
      defaultGarbagePiles: "1K",

      load: N => {
        garbage = new Array(N);
      },
      unload: () => {
        garbage = [];
        garbageIndex = 0;
      },
      makeGarbage: N => {
        var a = [];
        for (var i = 0; i < N; i++) {
          a[i] = N - i;
        }
        garbage[garbageIndex++] = a;
        if (garbageIndex == garbage.length) {
          garbageIndex = 0;
        }
      },
    };
  })()
);
