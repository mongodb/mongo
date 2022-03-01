/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

tests.set(
  "selfCyclicWeakMap",
  (function() {
    var garbage = [];
    var garbageIndex = 0;
    return {
      description: "var wm = new WeakMap(); wm[k1] = k2; wm[k2] = k3; ...",

      defaultGarbagePerFrame: "10K",
      defaultGarbagePiles: "1K",

      load: N => {
        garbage = new Array(N);
      },

      unload: () => {
        garbage = [];
        garbageIndex = 0;
      },

      makeGarbage: M => {
        var wm = new WeakMap();
        var initialKey = {};
        var key = initialKey;
        var value = {};
        for (var i = 0; i < M; i++) {
          wm.set(key, value);
          key = value;
          value = {};
        }
        garbage[garbageIndex++] = [initialKey, wm];
        if (garbageIndex == garbage.length) {
          garbageIndex = 0;
        }
      },
    };
  })()
);
