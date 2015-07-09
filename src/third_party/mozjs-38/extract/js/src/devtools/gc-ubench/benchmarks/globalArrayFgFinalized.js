window.tests.set('globalArrayFgFinalized', (function() {
var garbage = [];
var garbageIndex = 0;
return {
    description: "var foo = [ new Map, new Map, ... ]; # (foreground finalized)",

    load: (N) => { garbage = new Array(N); },
    unload: () => { garbage = []; garbageIndex = 0; },

    defaultGarbageTotal: "8K",
    defaultGarbagePerFrame: "1M",

    makeGarbage: (N) => {
        var arr = [];
        for (var i = 0; i < N; i++) {
            arr.push(new Map);
        }
        garbage[garbageIndex++] = arr;
        if (garbageIndex == garbage.length)
            garbageIndex = 0;
    }
};
})());
