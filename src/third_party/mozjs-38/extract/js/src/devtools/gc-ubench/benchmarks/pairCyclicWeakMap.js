window.tests.set('pairCyclicWeakMap', (function() {
var garbage = [];
var garbageIndex = 0;
return {
    description: "wm1[k1] = k2; wm2[k2] = k3; wm1[k3] = k4; wm2[k4] = ...",

    defaultGarbagePerFrame: "1K",
    defaultGarbageTotal: "1K",

    load: (N) => { garbage = new Array(N); },

    unload: () => { garbage = []; garbageIndex = 0; },

    makeGarbage: (M) => {
        var wm1 = new WeakMap();
        var wm2 = new WeakMap();
        var initialKey = {};
        var key = initialKey;
        var value = {};
        for (var i = 0; i < M/2; i++) {
            wm1.set(key, value);
            key = value;
            value = {};
            wm2.set(key, value);
            key = value;
            value = {};
        }
        garbage[garbageIndex++] = [ initialKey, wm1, wm2 ];
        if (garbageIndex == garbage.length)
            garbageIndex = 0;
    }
};
})());
