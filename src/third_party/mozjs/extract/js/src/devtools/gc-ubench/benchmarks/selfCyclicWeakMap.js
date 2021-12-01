window.tests.set('selfCyclicWeakMap', (function() {
var garbage = [];
var garbageIndex = 0;
return {
    description: "var wm = new WeakMap(); wm[k1] = k2; wm[k2] = k3; ...",

    defaultGarbagePerFrame: "1K",
    defaultGarbageTotal: "1K",

    load: (N) => { garbage = new Array(N); },

    unload: () => { garbage = []; garbageIndex = 0; },

    makeGarbage: (M) => {
        var wm = new WeakMap();
        var initialKey = {};
        var key = initialKey;
        var value = {};
        for (var i = 0; i < M; i++) {
            wm.set(key, value);
            key = value;
            value = {};
        }
        garbage[garbageIndex++] = [ initialKey, wm ];
        if (garbageIndex == garbage.length)
            garbageIndex = 0;
    }
};
})());
