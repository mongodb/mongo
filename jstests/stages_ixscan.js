// Test basic query stage index scan functionality.
t = db.stages_ixscan;
t.drop();

var N = 50;
for (var i = 0; i < N; ++i) {
    t.insert({foo: i, bar: N - i, baz: i});
}

t.ensureIndex({foo: 1})
t.ensureIndex({foo: 1, baz: 1});

// foo <= 20
ixscan1 = {ixscan: {args:{name: "stages_ixscan", keyPattern:{foo: 1},
                          startKey: {"": 20},
                          endKey: {}, endKeyInclusive: true,
                          direction: -1}}};
res = db.runCommand({stageDebug: ixscan1});
assert(!db.getLastError());
assert.eq(res.ok, 1);
assert.eq(res.results.length, 21);

// 20 <= foo < 30
ixscan1 = {ixscan: {args:{name: "stages_ixscan", keyPattern:{foo: 1},
                          startKey: {"": 20},
                          endKey: {"" : 30}, endKeyInclusive: false,
                          direction: 1}}};
res = db.runCommand({stageDebug: ixscan1});
assert(!db.getLastError());
assert.eq(res.ok, 1);
assert.eq(res.results.length, 10);

// 20 <= foo <= 30
ixscan1 = {ixscan: {args:{name: "stages_ixscan", keyPattern:{foo: 1},
                          startKey: {"": 20},
                          endKey: {"" : 30}, endKeyInclusive: true,
                          direction: 1}}};
res = db.runCommand({stageDebug: ixscan1});
assert(!db.getLastError());
assert.eq(res.ok, 1);
assert.eq(res.results.length, 11);

// 20 <= foo <= 30
// foo == 25
ixscan1 = {ixscan: {args:{name: "stages_ixscan", keyPattern:{foo: 1},
                          startKey: {"": 20},
                          endKey: {"" : 30}, endKeyInclusive: true,
                          direction: 1},
                    filter: {foo: 25}}};
res = db.runCommand({stageDebug: ixscan1});
assert(!db.getLastError());
assert.eq(res.ok, 1);
assert.eq(res.results.length, 1);

// 20 <= foo <= 30
// baz == 25 (in index so we can match against it.)
ixscan1 = {ixscan: {args:{name: "stages_ixscan", keyPattern:{foo:1, baz: 1},
                          startKey: {"": 20, "":MinKey},
                          endKey: {"" : 30, "":MaxKey}, endKeyInclusive: true,
                          direction: 1},
                    filter: {baz: 25}}};
res = db.runCommand({stageDebug: ixscan1});
assert(!db.getLastError());
assert.eq(res.ok, 1);
assert.eq(res.results.length, 1);

// 20 <= foo <= 30
// bar == 25 (not covered, should error.)
ixscan1 = {ixscan: {args:{name: "stages_ixscan", keyPattern:{foo:1, baz: 1},
                          startKey: {"": 20, "":MinKey},
                          endKey: {"" : 30, "":MaxKey}, endKeyInclusive: true,
                          direction: 1},
                    filter: {bar: 25}}};
res = db.runCommand({stageDebug: ixscan1});
assert(db.getLastError());
assert.eq(res.ok, 0);

// Try 2dsphere.
t.drop();

Random.setRandomSeed();
var random = Random.rand;
function sign() { return random() > 0.5 ? 1 : -1; }
var minDist = 0;
var maxDist = 1;
for(var i = 0; i < N; i++){
    var lat = sign() * (minDist + random() * (maxDist - minDist));
    var lng = sign() * (minDist + random() * (maxDist - minDist));
    var point = { geo: [lng, lat]};
    t.insert(point);
    assert(!db.getLastError());
}
t.ensureIndex({geo: "2dsphere"});

ixscan1 = {ixscan: {args:{name: "stages_ixscan", keyPattern:{geo: "2dsphere"},
                          startKey: {geo: {$geoNear: {$geometry: {type: "Point", coordinates:[0,0]}}}},
                          endKey: {}, endKeyInclusive: false,
                          direction: 1}}};
res = db.runCommand({stageDebug: ixscan1});
assert(!db.getLastError());
assert.eq(res.ok, 1);
assert.eq(res.results.length, N);

// Also, try 2d.
t.dropIndex({geo: "2dsphere"});
t.ensureIndex({geo: "2d"});
ixscan1 = {ixscan: {args:{name: "stages_ixscan", keyPattern:{geo: "2d"},
                          startKey: {geo: {$near: [0,0]}},
                          endKey: {}, endKeyInclusive: false,
                          direction: 1, limit: 100}}};
res = db.runCommand({stageDebug: ixscan1});
assert(!db.getLastError());
assert.eq(res.ok, 1);
assert.eq(res.results.length, N);
