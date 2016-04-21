window.tests.set('globalArrayArrayLiteral', (function() {
var garbage = [];
var garbageIndex = 0;
return {
    description: "var foo = [[], ....]",
    load: (N) => { garbage = new Array(N); },
    unload: () => { garbage = []; garbageIndex = 0; },
    makeGarbage: (N) => {
        for (var i = 0; i < N; i++) {
            garbage[garbageIndex++] = ['foo', 'bar', 'baz', 'baa'];
            if (garbageIndex == garbage.length)
                garbageIndex = 0;
        }
    }
};
})());
