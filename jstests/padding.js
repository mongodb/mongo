p = db.getCollection("padding");
p.drop();

for (var i = 0; i < 1000; i++) {
    p.insert({ x: 1, y: "aaaaaaaaaaaaaaa" });
}

assert(p.stats().paddingFactor == 1);

for (var i = 0; i < 1000; i++) {
    var x = p.findOne();
    x.y = x.y + "aaaaaaaaaaaaaaaa";
    p.update({}, x);
    if (i % 100 == 0)

        print(p.stats().paddingFactor);
}

assert(p.stats().paddingFactor > 1.9);

// this should make it go down
for (var i = 0; i < 1000; i++) {
    p.update({}, { $inc: { x: 1} });
    if (i % 100 == 0)
        print(p.stats().paddingFactor);
}
assert(p.stats().paddingFactor < 1.7);

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
assert(ps > 1.7 && ps < 1.9);

// 50/50 inserts and nonfitting updates
for (var i = 0; i < 1000; i++) {
    if (i % 2 == 0) {
        p.insert({});
    }
    else {
        var x = p.findOne();
        x.y = x.y + "aaaaaaaaaaaaaaaa";
        p.update({}, x);
    }
    if (i % 100 == 0)
        print(p.stats().paddingFactor);
}

// should have trended somewhat higher over the above. 
// speed of increase would be higher with more indexes.
assert(p.stats().paddingFactor > ps + 0.02 , "now: " + p.stats().paddingFactor + " ps: " + ps );
p.drop();
