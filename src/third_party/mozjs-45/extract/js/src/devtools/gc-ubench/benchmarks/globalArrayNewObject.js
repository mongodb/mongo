window.tests.set('globalArrayNewObject', (function() {
var garbage = [];
var garbageIndex = 0;
return {
    description: "var foo = [new Object(), ....]",
    load: (N) => { garbage = new Array(N); },
    unload: () => { garbage = []; garbageIndex = 0; },
    makeGarbage: (N) => {
        for (var i = 0; i < N; i++) {
            garbage[garbageIndex++] = new Object();
            if (garbageIndex == garbage.length)
                garbageIndex = 0;
        }
    }
};
})());
