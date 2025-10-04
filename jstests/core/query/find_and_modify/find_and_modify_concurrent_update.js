// @tags: [
//   # Aggregation (used for writes without shard key) does not support $where.
//   assumes_unsharded_collection,
//   # Uses $where operator
//   requires_scripting,
//   uses_multiple_connections,
//   uses_parallel_shell,
// ]

// Ensures that find and modify will not apply an update to a document which, due to a concurrent
// modification, no longer matches the query predicate.
// Repeat the test a few times, as the timing of the yield means it won't fail consistently.
for (let i = 0; i < 3; i++) {
    const t = db[jsTestName()];
    t.drop();

    assert.commandWorked(t.createIndex({a: 1}));
    assert.commandWorked(t.createIndex({b: 1}));
    assert.commandWorked(t.insert({_id: 1, a: 1, b: 1}));

    const join = startParallelShell("db.find_and_modify_concurrent.update({a: 1, b: 1}, {$inc: {a: 1}});");

    // Due to the sleep, we expect this find and modify to yield before updating the
    // document.
    const res = t.findAndModify({query: {a: 1, b: 1, $where: "sleep(100); return true;"}, update: {$inc: {a: 1}}});

    join();
    const docs = t.find().toArray();
    assert.eq(docs.length, 1);

    // Both the find and modify and the update operations look for a document with a==1,
    // and then increment 'a' by 1. One should win the race and set a=2. The other should
    // fail to find a match. The assertion is that 'a' got incremented once (not zero times
    // and not twice).
    assert.eq(docs[0].a, 2);
}
