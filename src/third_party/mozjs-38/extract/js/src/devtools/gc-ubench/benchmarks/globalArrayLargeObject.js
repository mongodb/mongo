window.tests.set('globalArrayLargeObject', (function() {
var garbage = [];
var garbageIndex = 0;
return {
    description: "var foo = { LARGE }; # (large slots)",

    load: (N) => { garbage = new Array(N); },
    unload: () => { garbage = []; garbageIndex = 0; },

    defaultGarbageTotal: "8K",
    defaultGarbagePerFrame: "200K",

    makeGarbage: (N) => {
        var obj = {};
        for (var i = 0; i < N; i++) {
            obj["key" + i] = i;
        }
        garbage[garbageIndex++] = obj;
        if (garbageIndex == garbage.length)
            garbageIndex = 0;
    }
};
})());
