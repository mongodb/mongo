window.tests.set('bigTextNodes', (function() {
var garbage = [];
var garbageIndex = 0;
return {
    description: "var foo = [ textNode, textNode, ... ]",

    load: (N) => { garbage = new Array(N); },
    unload: () => { garbage = []; garbageIndex = 0; },

    defaultGarbagePerFrame: "8",
    defaultGarbageTotal: "8",

    makeGarbage: (N) => {
        var a = [];
        var s = "x";
        for (var i = 0; i < 16; i++)
            s = s + s;
        for (var i = 0; i < N; i++)
            a.push(document.createTextNode(s));
        garbage[garbageIndex++] = a;
        if (garbageIndex == garbage.length)
            garbageIndex = 0;
    }
};
})());
