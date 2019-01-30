window.tests.set('globalArrayBuffer', (function() {
var garbage = [];
var garbageIndex = 0;
return {
    description: "var foo = ArrayBuffer(N); # (large malloc data)",

    load: (N) => { garbage = new Array(N); },
    unload: () => { garbage = []; garbageIndex = 0; },

    defaultGarbageTotal: "8K",
    defaultGarbagePerFrame: "4M",

    makeGarbage: (N) => {
        var ab = new ArrayBuffer(N);
        var view = new Uint8Array(ab);
        view[0] = 1;
        view[N - 1] = 2;
        garbage[garbageIndex++] = ab;
        if (garbageIndex == garbage.length)
            garbageIndex = 0;
    }
};
})());
