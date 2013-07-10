// Test basic query stage collection scan functionality.
t = db.stages_collection_scan;
t.drop();

var N = 50;
for (var i = 0; i < N; ++i) {
    t.insert({foo: i});
}

forward = {cscan: {args: {name: "stages_collection_scan", direction: 1}}}
res = db.runCommand({stageDebug: forward});
assert(!db.getLastError());
assert.eq(res.ok, 1);
assert.eq(res.results.length, N);
assert.eq(res.results[0].foo, 0);
assert.eq(res.results[49].foo, 49);

// And, backwards.
backward = {cscan: {args: {name: "stages_collection_scan", direction: -1}}}
res = db.runCommand({stageDebug: backward});
assert(!db.getLastError());
assert.eq(res.ok, 1);
assert.eq(res.results.length, N);
assert.eq(res.results[0].foo, 49);
assert.eq(res.results[49].foo, 0);

forwardFiltered  = {cscan: {args: {name: "stages_collection_scan", direction: 1},
                            filter: {foo: {$lt: 25}}}}
res = db.runCommand({stageDebug: forwardFiltered});
assert(!db.getLastError());
assert.eq(res.ok, 1);
assert.eq(res.results.length, 25);
assert.eq(res.results[0].foo, 0);
assert.eq(res.results[24].foo, 24);

backwardFiltered  = {cscan: {args: {name: "stages_collection_scan", direction: -1},
                             filter: {foo: {$lt: 25}}}}
res = db.runCommand({stageDebug: backwardFiltered});
assert(!db.getLastError());
assert.eq(res.ok, 1);
assert.eq(res.results.length, 25);
assert.eq(res.results[0].foo, 24);
assert.eq(res.results[24].foo, 0);
