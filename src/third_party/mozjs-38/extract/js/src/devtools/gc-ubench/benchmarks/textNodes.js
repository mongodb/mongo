window.tests.set('textNodes', (function() {
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
            a.push(document.createTextNode("t" + i));
        }
        garbage[garbageIndex++] = a;
        if (garbageIndex == garbage.length)
            garbageIndex = 0;
    }
};
})());
