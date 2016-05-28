
t = db.geo3;
t.drop();

n = 1;
arr = [];
for (var x = -100; x < 100; x += 2) {
    for (var y = -100; y < 100; y += 2) {
        arr.push({_id: n++, loc: [x, y], a: Math.abs(x) % 5, b: Math.abs(y) % 5});
    }
}
t.insert(arr);
assert.eq(t.count(), 100 * 100);
assert.eq(t.count(), n - 1);

t.ensureIndex({loc: "2d"});

fast = db.runCommand({geoNear: t.getName(), near: [50, 50], num: 10});

// test filter

filtered1 = db.runCommand({geoNear: t.getName(), near: [50, 50], num: 10, query: {a: 2}});
assert.eq(10, filtered1.results.length, "B1");
filtered1.results.forEach(function(z) {
    assert.eq(2, z.obj.a, "B2: " + tojson(z));
});
// printjson( filtered1.stats );

function avgA(q, len) {
    if (!len)
        len = 10;
    var realq = {loc: {$near: [50, 50]}};
    if (q)
        Object.extend(realq, q);
    var as = t.find(realq).limit(len).map(function(z) {
        return z.a;
    });
    assert.eq(len, as.length, "length in avgA");
    return Array.avg(as);
}

function testFiltering(msg) {
    assert.gt(2, avgA({}), msg + " testFiltering 1 ");
    assert.eq(2, avgA({a: 2}), msg + " testFiltering 2 ");
    assert.eq(4, avgA({a: 4}), msg + " testFiltering 3 ");
}

testFiltering("just loc");

t.dropIndex({loc: "2d"});
assert.eq(1, t.getIndexKeys().length, "setup 3a");
t.ensureIndex({loc: "2d", a: 1});
assert.eq(2, t.getIndexKeys().length, "setup 3b");

filtered2 = db.runCommand({geoNear: t.getName(), near: [50, 50], num: 10, query: {a: 2}});
assert.eq(10, filtered2.results.length, "B3");
filtered2.results.forEach(function(z) {
    assert.eq(2, z.obj.a, "B4: " + tojson(z));
});

assert.eq(filtered1.stats.avgDistance, filtered2.stats.avgDistance, "C1");
assert.gt(filtered1.stats.objectsLoaded, filtered2.stats.objectsLoaded, "C3");

testFiltering("loc and a");

t.dropIndex({loc: "2d", a: 1});
assert.eq(1, t.getIndexKeys().length, "setup 4a");
t.ensureIndex({loc: "2d", b: 1});
assert.eq(2, t.getIndexKeys().length, "setup 4b");

testFiltering("loc and b");

q = {
    loc: {$near: [50, 50]}
};
assert.eq(100, t.find(q).limit(100).itcount(), "D1");
assert.eq(100, t.find(q).limit(100).size(), "D2");

assert.eq(20, t.find(q).limit(20).itcount(), "D3");
assert.eq(20, t.find(q).limit(20).size(), "D4");

// SERVER-14039 Wrong limit after skip with $nearSphere, 2d index
assert.eq(10, t.find(q).skip(10).limit(10).itcount(), "D5");
assert.eq(10, t.find(q).skip(10).limit(10).size(), "D6");
