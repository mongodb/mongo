load('jstests/libs/parallelTester.js');

var start = new Date();
print("start: " + start);
var func = function() {
    db.runCommand({sleep: 1, seconds: 10000});
    return new Date();
};
a = new Thread(func);
b = new Thread(func);
a.start();
b.start();
a.join();
b.join();
assert.lt(a.returnData() - start, 15000, "A took more than 15s");
assert.lt(b.returnData() - start, 15000, "B took more than 15s");
