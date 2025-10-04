// Cannot implicitly shard accessed collections because of collection existing when none
// expected.
// @tags: [
//   assumes_no_implicit_collection_creation_after_drop,
//   requires_capped,
//   requires_getmore,
//   # This test relies on query commands returning specific batch-sized responses.
//   assumes_no_implicit_cursor_exhaustion,
// ]

// Test attaching maxTimeMS to a getMore command.
let cmdRes;
let collName = "getmore_cmd_maxtimems";
let coll = db[collName];
coll.drop();

for (var i = 0; i < 10; i++) {
    assert.commandWorked(coll.insert({a: i}));
}

// Can't attach maxTimeMS to a getMore command for a non-tailable cursor over a non-capped
// collection.
cmdRes = db.runCommand({find: collName, batchSize: 2});
assert.commandWorked(cmdRes);
cmdRes = db.runCommand({getMore: cmdRes.cursor.id, collection: collName, maxTimeMS: 60000});
assert.commandFailed(cmdRes);

coll.drop();
assert.commandWorked(db.createCollection(collName, {capped: true, size: 1024}));
for (var i = 0; i < 10; i++) {
    assert.commandWorked(coll.insert({a: i}));
}

// Can't attach maxTimeMS to a getMore command for a non-tailable cursor over a capped
// collection.
cmdRes = db.runCommand({find: collName, batchSize: 2});
assert.commandWorked(cmdRes);
cmdRes = db.runCommand({getMore: cmdRes.cursor.id, collection: collName, maxTimeMS: 60000});
assert.commandFailed(cmdRes);

// Can't attach maxTimeMS to a getMore command for a non-awaitData tailable cursor.
cmdRes = db.runCommand({find: collName, batchSize: 2, tailable: true});
assert.commandWorked(cmdRes);
cmdRes = db.runCommand({getMore: cmdRes.cursor.id, collection: collName, maxTimeMS: 60000});
assert.commandFailed(cmdRes);

// Can attach maxTimeMS to a getMore command for an awaitData cursor.
cmdRes = db.runCommand({find: collName, batchSize: 2, tailable: true, awaitData: true});
assert.commandWorked(cmdRes);
cmdRes = db.runCommand({getMore: cmdRes.cursor.id, collection: collName, maxTimeMS: 60000});
assert.commandWorked(cmdRes);
