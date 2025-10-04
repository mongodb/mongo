/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

tests.set(
  "bigTextNodes",
  (function() {
    var garbage = [];
    var garbageIndex = 0;
    return {
      description: "var foo = [ textNode, textNode, ... ]",

      enabled: "document" in globalThis,

      load: N => {
        garbage = new Array(N);
      },
      unload: () => {
        garbage = [];
        garbageIndex = 0;
      },

      defaultGarbagePerFrame: "8",
      defaultGarbagePiles: "8",

      makeGarbage: N => {
        var a = [];
        var s = "x";
        for (var i = 0; i < 16; i++) {
          s = s + s;
        }
        for (var i = 0; i < N; i++) {
          a.push(document.createTextNode(s));
        }
        garbage[garbageIndex++] = a;
        if (garbageIndex == garbage.length) {
          garbageIndex = 0;
        }
      },
    };
  })()
);
