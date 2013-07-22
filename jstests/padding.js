p = db.getCollection("padding");
p.drop();

for (var i = 0; i < 1000; i++) {
    p.insert({ x: 1, y: "aaaaaaaaaaaaaaa" });
}

assert.eq(p.stats().paddingFactor, 1, "Padding Not 1");

for (var i = 0; i < 1000; i++) {
    var x = p.findOne();
    x.y = x.y + "aaaaaaaaaaaaaaaa";
    p.update({}, x);
    if (i % 100 == 0)

        print(p.stats().paddingFactor);
}

assert.gt(p.stats().paddingFactor, 1.9, "Padding not > 1.9");

// this should make it go down
for (var i = 0; i < 1000; i++) {
    p.update({}, { $inc: { x: 1} });
    if (i % 100 == 0)
        print(p.stats().paddingFactor);
}
assert.lt(p.stats().paddingFactor, 1.7, "Padding not < 1.7");

for (var i = 0; i < 1000; i++) {
    if (i % 2 == 0) {
        p.update({}, { $inc: { x: 1} });
    }
    else {
        var x = p.findOne();
        x.y = x.y + "aaaaaaaaaaaaaaaa";
        p.update({}, x);
    }
    if( i % 100 == 0 )
        print(p.stats().paddingFactor);
}
var ps = p.stats().paddingFactor;
assert.gt(ps, 1.7, "Padding not greater than 1.7");
assert.lt(ps, 1.9, "Padding not less than 1.9");

// 50/50 inserts and nonfitting updates
for (var i = 0; i < 1000; i++) {
    if (i % 2 == 0) {
        p.insert({});
    }
    else {
        var x = p.findOne();
        x.y = x.y + "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        p.update({}, x);
    }
    if (i % 100 == 0)
        print(p.stats().paddingFactor);
}

// should have trended somewhat higher over the above.
// speed of increase would be higher with more indexes.
assert.gt(p.stats().paddingFactor, ps + 0.02 , "padding factor not greater than value (+.02)");
p.drop();
