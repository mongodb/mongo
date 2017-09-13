window.tests.set('events', (function() {
var garbage = [];
var garbageIndex = 0;
return {
    description: "var foo = [ textNode, textNode, ... ]",

    load: (N) => { garbage = new Array(N); },
    unload: () => { garbage = []; garbageIndex = 0; },

    defaultGarbagePerFrame: "100K",
    defaultGarbageTotal: "8",

    makeGarbage: (N) => {
        var a = [];
        for (var i = 0; i < N; i++) {
            var e = document.createEvent("Events");
            e.initEvent("TestEvent", true, true);
            a.push(e);
        }
        garbage[garbageIndex++] = a;
        if (garbageIndex == garbage.length)
            garbageIndex = 0;
    }
};
})());
