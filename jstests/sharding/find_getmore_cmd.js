/**
 * Test issuing raw find and getMore commands to mongos using db.runCommand().
 */
(function() {
    "use strict";

    var cmdRes;
    var cursorId;

    var st = new ShardingTest({shards: 2});
    st.stopBalancer();

    // Set up a collection sharded by "_id" with one chunk on each of the two shards.
    var db = st.s.getDB("test");
    var coll = db.getCollection("find_getmore_cmd");

    coll.drop();
    assert.writeOK(coll.insert({_id: -9}));
    assert.writeOK(coll.insert({_id: -5}));
    assert.writeOK(coll.insert({_id: -1}));
    assert.writeOK(coll.insert({_id: 1}));
    assert.writeOK(coll.insert({_id: 5}));
    assert.writeOK(coll.insert({_id: 9}));

    assert.commandWorked(db.adminCommand({enableSharding: db.getName()}));
    st.ensurePrimaryShard(db.getName(), "shard0000");
    db.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}});
    assert.commandWorked(db.adminCommand({split: coll.getFullName(), middle: {_id: 0}}));
    assert.commandWorked(db.adminCommand({
        moveChunk: coll.getFullName(),
        find: {_id: 1},
        to: "shard0001"
    }));

    // Find with no options.
    cmdRes = db.runCommand({find: coll.getName()});
    assert.commandWorked(cmdRes);
    assert.eq(cmdRes.cursor.id, NumberLong(0));
    assert.eq(cmdRes.cursor.ns, coll.getFullName());
    assert.eq(cmdRes.cursor.firstBatch.length, 6);

    // Find with batchSize greater than the number of docs residing on each shard. This means that a
    // getMore is required between mongos and the shell, but no getMores are issued between mongos
    // and mongod.
    cmdRes = db.runCommand({find: coll.getName(), batchSize: 4});
    assert.commandWorked(cmdRes);
    assert.gt(cmdRes.cursor.id, NumberLong(0));
    assert.eq(cmdRes.cursor.ns, coll.getFullName());
    assert.eq(cmdRes.cursor.firstBatch.length, 4);
    cmdRes = db.runCommand({getMore: cmdRes.cursor.id, collection: coll.getName()});
    assert.commandWorked(cmdRes);
    assert.eq(cmdRes.cursor.id, NumberLong(0));
    assert.eq(cmdRes.cursor.ns, coll.getFullName());
    assert.eq(cmdRes.cursor.nextBatch.length, 2);

    // Find with batchSize less than the number of docs residing on each shard. This time getMores
    // will be issued between mongos and mongod.
    cmdRes = db.runCommand({find: coll.getName(), batchSize: 2});
    assert.commandWorked(cmdRes);
    assert.gt(cmdRes.cursor.id, NumberLong(0));
    assert.eq(cmdRes.cursor.ns, coll.getFullName());
    assert.eq(cmdRes.cursor.firstBatch.length, 2);
    cursorId = cmdRes.cursor.id;
    cmdRes = db.runCommand({getMore: cursorId, collection: coll.getName(), batchSize: 2});
    assert.commandWorked(cmdRes);
    assert.eq(cmdRes.cursor.id, cursorId);
    assert.eq(cmdRes.cursor.ns, coll.getFullName());
    assert.eq(cmdRes.cursor.nextBatch.length, 2);
    cmdRes = db.runCommand({getMore: cursorId, collection: coll.getName()});
    assert.eq(cmdRes.cursor.id, NumberLong(0));
    assert.eq(cmdRes.cursor.ns, coll.getFullName());
    assert.eq(cmdRes.cursor.nextBatch.length, 2);

    // Combine skip, limit, and sort.
    cmdRes = db.runCommand({find: coll.getName(), skip: 4, limit: 1, sort: {_id: -1}});
    assert.commandWorked(cmdRes);
    assert.eq(cmdRes.cursor.id, NumberLong(0));
    assert.eq(cmdRes.cursor.ns, coll.getFullName());
    assert.eq(cmdRes.cursor.firstBatch.length, 1);
    assert.eq(cmdRes.cursor.firstBatch[0], {_id: -5});

    st.stop();
})();
