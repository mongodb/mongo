
t = db.mr_bench;
t.drop();

function getRandomStr(L) {
    var s = '';
    var randomchar = function() {
        var n = Math.floor(Math.random() * 62);
        if (n < 10)
            return n;  // 1-10
        if (n < 36)
            return String.fromCharCode(n + 55);  // A-Z
        return String.fromCharCode(n + 61);      // a-z
    };
    while (s.length < L)
        s += randomchar();
    return s;
}

t.ensureIndex({rand: 1}, {unique: true});

largeStr = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
while (largeStr.length < 512) {
    largeStr += largeStr;
}
largeStr = largeStr.substr(512);

for (i = 0; i < 100000; ++i) {
    t.save({rand: getRandomStr(20), same: "the same string", str: largeStr});
}

emit = printjson;
count = t.count();

function d(x) {
    printjson(x);
}

m = function() {
    emit(this.rand, {id: this._id, str: this.str});
};

m2 = function() {
    emit(this.same, this.rand);
};

r = function(k, vals) {
    var tmp = {};
    vals.forEach(function(i) {
        if (typeof(i) == 'string') {
            tmp[i] = true;
        } else {
            for (var z in i)
                tmp[z] = true;
        }
    });

    return tmp;
};

// following time limits are passing fine on a laptop with a debug build
// so should always pass in theory unless something is wrong: GC, too much reducing, etc

// 1st MR just uses random unique keys, with no reduce involved
// this should be straightforward for perf, but could lead to OOM if settings are bad
assert.time(function() {
    res = db.runCommand({mapreduce: "mr_bench", map: m, reduce: r, out: "mr_bench_out"});
    d(res);
    assert.eq(count, res.counts.input, "A");
    x = db[res.result];
    assert.eq(count, x.find().count(), "B");
    return 1;
}, "unique key mr", 15000);

// 2nd MR emits the same key, and a unique value is added as key to same object
// if object is kept in ram and being reduced, this can be really slow
assert.time(function() {
    res = db.runCommand({mapreduce: "mr_bench", map: m2, reduce: r, out: "mr_bench_out"});
    d(res);
    assert.eq(count, res.counts.input, "A");
    x = db[res.result];
    assert.eq(1, x.find().count(), "B");
    return 1;
}, "single key mr", 20000);
