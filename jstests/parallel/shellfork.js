load('jstests/libs/parallelTester.js');

a = fork(function(a, b) {
    return a / b;
}, 10, 2);
a.start();
b = fork(function(a, b, c) {
    return a + b + c;
}, 18, " is a ", "multiple of 3");
makeFunny = function(text) {
    return text + " ha ha!";
};
c = fork(makeFunny, "paisley");
c.start();
b.start();
b.join();
assert.eq(5, a.returnData());
assert.eq("18 is a multiple of 3", b.returnData());
assert.eq("paisley ha ha!", c.returnData());

z = fork(function(a) {
    load('jstests/libs/parallelTester.js');
    var y = fork(function(a) {
        return a + 1;
    }, 5);
    y.start();
    return y.returnData() + a;
}, 1);
z.start();
assert.eq(7, z.returnData());

t = 1;
z = new ScopedThread(function() {
    assert(typeof(t) == "undefined", "t not undefined");
    t = 5;
    return t;
});
z.start();
assert.eq(5, z.returnData());
assert.eq(1, t);