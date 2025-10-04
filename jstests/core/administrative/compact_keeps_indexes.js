// SERVER-16676 Make sure compact doesn't leave the collection with bad indexes
// SERVER-16967 Make sure compact doesn't crash while collections are being dropped
// in a different database.
//
// @tags: [
//   # The test runs commands that are not allowed with security token: compact.
//   not_allowed_with_signed_security_token,
//   uses_multiple_connections,
//   uses_parallel_shell,
//   uses_compact,
// ]

let coll = db.compact_keeps_indexes;

coll.drop();
coll.insert({_id: 1, x: 1});
coll.createIndex({x: 1});

assert.eq(coll.getIndexes().length, 2);

// force:true is for replset passthroughs
let res = coll.runCommand("compact", {force: true});
// Some storage engines (for example, inMemoryExperiment) do not support the compact command.
if (res.code == 115) {
    quit();
}
assert.commandWorked(res);

assert.eq(coll.getIndexes().length, 2);
assert.eq(coll.find({_id: 1}).itcount(), 1);
assert.eq(coll.find({x: 1}).itcount(), 1);

let dropCollectionShell = startParallelShell(function () {
    let t = db.getSiblingDB("test_compact_keeps_indexes_drop").testcoll;
    t.drop();
    for (let i = 0; i < 100; i++) {
        t.save({a: 1});
        t.drop();
    }
});
for (let i = 0; i < 10; i++) {
    coll.runCommand("compact");
}
dropCollectionShell();
