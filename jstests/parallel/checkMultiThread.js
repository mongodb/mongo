load('jstests/libs/parallelTester.js');

var start = new Date();
print("start: " + start);
var func = function() {
    db.runCommand({$eval: "sleep(10000);", nolock: true});
    return new Date();
};
a = new ScopedThread(func);
b = new ScopedThread(func);
a.start();
b.start();
a.join();
b.join();
assert.lt(a.returnData() - start, 15000, "A took more than 15s");
assert.lt(b.returnData() - start, 15000, "B took more than 15s");
