window.tests.set('propertyTreeSplitting', (function() {
var garbage = [];
var garbageIndex = 0;
return {
    description: "use delete to generate Shape garbage",
    load: (N) => { garbage = new Array(N); },
    unload: () => { garbage = []; garbageIndex = 0; },
    makeGarbage: (N) => {
        function f()
        {
            var a1 = eval;
            delete eval;
            eval = a1;
            var a3 = toString;
            delete toString;
            toString = a3;
        }
        for (var a = 0; a < N; ++a) {
            garbage[garbageIndex++] = new f();
            if (garbageIndex == garbage.length)
                garbageIndex = 0;
        }
    }
};
})());
