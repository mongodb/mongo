// dur1.js

print("dur1.js");

var name = "dur1";

print(1);

var n = startMongodTest(31001, name + "-nodur", 0, {});

print(2);

var d = startMongodTest(31002, name + "-dur", 0, { dur: true });

print(3);

//assert(n.foo.count() == 0);

print(4);

//assert(d.foo.count() == 0);

print("SUCCESS dur1.js");
