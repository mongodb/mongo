let t = db.fts_partition_no_multikey;
t.drop();

t.createIndex({x: 1, y: "text"});

assert.commandWorked(t.insert({x: 5, y: "this is fun"}));

assert.writeError(t.insert({x: [], y: "this is fun"}));

assert.writeError(t.insert({x: [1], y: "this is fun"}));

assert.writeError(t.insert({x: [1, 2], y: "this is fun"}));
