/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

tests.set(
  "events",
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

      defaultGarbagePerFrame: "100K",
      defaultGarbagePiles: "8",

      makeGarbage: N => {
        var a = [];
        for (var i = 0; i < N; i++) {
          var e = document.createEvent("Events");
          e.initEvent("TestEvent", true, true);
          a.push(e);
        }
        garbage[garbageIndex++] = a;
        if (garbageIndex == garbage.length) {
          garbageIndex = 0;
        }
      },
    };
  })()
);
