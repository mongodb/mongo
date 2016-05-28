
asdf = db.getCollection("asdf");
asdf.drop();

var txt = "asdf";
for (var i = 0; i < 10; i++) {
    txt = txt + txt;
}

var iterations = _isWindows() ? 2500 : 5000;

// fill db
for (var i = 1; i <= iterations; i++) {
    var obj = {txt: txt};
    asdf.save(obj);

    var obj2 = {
        txt: txt,
        comments: [{num: i, txt: txt}, {num: [], txt: txt}, {num: true, txt: txt}]
    };
    asdf.update(obj, obj2);

    if (i % 100 == 0) {
        var c = asdf.count();
        assert.eq(c, i);
    }
}

assert(asdf.validate().valid);

var stats = db.runCommand({collstats: "asdf"});

// some checks. want to check that padding factor is working; in addition this lets us do a little
// basic
// testing of the collstats command at the same time
assert(stats.count == iterations);
assert(stats.size < 140433012 * 5 && stats.size > 1000000);
assert(stats.numExtents < 20);
assert(stats.nindexes == 1);

asdf.drop();
