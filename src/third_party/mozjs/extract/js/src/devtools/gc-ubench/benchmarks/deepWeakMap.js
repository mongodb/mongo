/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

tests.set(
  "deepWeakMap",
  (function() {
    var garbage = [];
    var garbageIndex = 0;
    return {
      description: "o={wm,k}; w.mk[k]=o2={wm2,k2}; wm2[k2]=....",

      defaultGarbagePerFrame: "1K",
      defaultGarbagePiles: "1K",

      load: N => {
        garbage = new Array(N);
      },

      unload: () => {
        garbage = [];
        garbageIndex = 0;
      },

      makeGarbage: M => {
        var initial = {};
        var prev = initial;
        for (var i = 0; i < M; i++) {
          var obj = [new WeakMap(), Object.create(null)];
          obj[0].set(obj[1], prev);
          prev = obj;
        }
        garbage[garbageIndex++] = initial;
        if (garbageIndex == garbage.length) {
          garbageIndex = 0;
        }
      },
    };
  })()
);
