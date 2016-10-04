
t = db.fts_partition_no_multikey;
t.drop();

t.ensureIndex({x: 1, y: "text"});

assert.writeOK(t.insert({x: 5, y: "this is fun"}));

assert.writeError(t.insert({x: [], y: "this is fun"}));

assert.writeError(t.insert({x: [1], y: "this is fun"}));

assert.writeError(t.insert({x: [1, 2], y: "this is fun"}));
