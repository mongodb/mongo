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
    assert.writeOK(coll.insert({_id: -9, a: 4, b: "foo foo"}));
    assert.writeOK(coll.insert({_id: -5, a: 8}));
    assert.writeOK(coll.insert({_id: -1, a: 10, b: "foo"}));
    assert.writeOK(coll.insert({_id: 1, a: 5}));
    assert.writeOK(coll.insert({_id: 5, a: 20, b: "foo foo foo"}));
    assert.writeOK(coll.insert({_id: 9, a: 3}));

    assert.commandWorked(coll.ensureIndex({b: "text"}));

    assert.commandWorked(db.adminCommand({enableSharding: db.getName()}));
    st.ensurePrimaryShard(db.getName(), "shard0000");
    db.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}});
    assert.commandWorked(db.adminCommand({split: coll.getFullName(), middle: {_id: 0}}));
    assert.commandWorked(
        db.adminCommand({moveChunk: coll.getFullName(), find: {_id: 1}, to: "shard0001"}));

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
    assert.eq(cmdRes.cursor.firstBatch[0], {_id: -5, a: 8});

    // Find where adding limit/ntoreturn and skip overflows.
    var largeInt = new NumberLong('9223372036854775807');
    cmdRes = db.runCommand({find: coll.getName(), skip: largeInt, limit: largeInt});
    assert.commandFailed(cmdRes);
    cmdRes = db.runCommand({find: coll.getName(), skip: largeInt, ntoreturn: largeInt});
    assert.commandFailed(cmdRes);
    cmdRes = db.runCommand(
        {find: coll.getName(), skip: largeInt, ntoreturn: largeInt, singleBatch: true});
    assert.commandFailed(cmdRes);

    // A predicate with $where.
    cmdRes = db.runCommand({find: coll.getName(), filter: {$where: "this._id == 5"}});
    assert.commandWorked(cmdRes);
    assert.eq(cmdRes.cursor.id, NumberLong(0));
    assert.eq(cmdRes.cursor.ns, coll.getFullName());
    assert.eq(cmdRes.cursor.firstBatch.length, 1);
    assert.eq(cmdRes.cursor.firstBatch[0], {_id: 5, a: 20, b: "foo foo foo"});

    // Tailable option should result in a failure because the collection is not capped.
    cmdRes = db.runCommand({find: coll.getName(), tailable: true});
    assert.commandFailed(cmdRes);

    // $natural sort.
    cmdRes = db.runCommand({find: coll.getName(), sort: {$natural: 1}});
    assert.commandWorked(cmdRes);
    assert.eq(cmdRes.cursor.id, NumberLong(0));
    assert.eq(cmdRes.cursor.ns, coll.getFullName());
    assert.eq(cmdRes.cursor.firstBatch.length, 6);

    // Should be able to sort despite projecting out the sort key.
    cmdRes = db.runCommand({find: coll.getName(), sort: {a: 1}, projection: {_id: 1}});
    assert.commandWorked(cmdRes);
    assert.eq(cmdRes.cursor.id, NumberLong(0));
    assert.eq(cmdRes.cursor.ns, coll.getFullName());
    assert.eq(cmdRes.cursor.firstBatch.length, 6);
    assert.eq(cmdRes.cursor.firstBatch[0], {_id: 9});
    assert.eq(cmdRes.cursor.firstBatch[1], {_id: -9});
    assert.eq(cmdRes.cursor.firstBatch[2], {_id: 1});
    assert.eq(cmdRes.cursor.firstBatch[3], {_id: -5});
    assert.eq(cmdRes.cursor.firstBatch[4], {_id: -1});
    assert.eq(cmdRes.cursor.firstBatch[5], {_id: 5});

    // Ensure textScore meta-sort works in mongos.
    cmdRes = db.runCommand({
        find: coll.getName(),
        filter: {$text: {$search: "foo"}},
        sort: {score: {$meta: "textScore"}},
        projection: {score: {$meta: "textScore"}}
    });
    assert.commandWorked(cmdRes);
    assert.eq(cmdRes.cursor.id, NumberLong(0));
    assert.eq(cmdRes.cursor.ns, coll.getFullName());
    assert.eq(cmdRes.cursor.firstBatch.length, 3);
    assert.eq(cmdRes.cursor.firstBatch[0]["_id"], 5);
    assert.eq(cmdRes.cursor.firstBatch[1]["_id"], -9);
    assert.eq(cmdRes.cursor.firstBatch[2]["_id"], -1);

    // User projection on $sortKey is illegal.
    cmdRes = db.runCommand({find: coll.getName(), projection: {$sortKey: 1}, sort: {_id: 1}});
    assert.commandFailed(cmdRes);
    cmdRes = db.runCommand(
        {find: coll.getName(), projection: {$sortKey: {$meta: 'sortKey'}}, sort: {_id: 1}});
    assert.commandFailed(cmdRes);

    // User should be able to issue a sortKey meta-projection, as long as it's not on the reserved
    // $sortKey field.
    cmdRes = db.runCommand({
        find: coll.getName(),
        projection: {_id: 0, a: 0, b: 0, key: {$meta: 'sortKey'}},
        sort: {_id: 1}
    });
    assert.commandWorked(cmdRes);
    assert.eq(cmdRes.cursor.id, NumberLong(0));
    assert.eq(cmdRes.cursor.ns, coll.getFullName());
    assert.eq(cmdRes.cursor.firstBatch.length, 6);
    assert.eq(cmdRes.cursor.firstBatch[0], {key: {"": -9}});
    assert.eq(cmdRes.cursor.firstBatch[1], {key: {"": -5}});
    assert.eq(cmdRes.cursor.firstBatch[2], {key: {"": -1}});
    assert.eq(cmdRes.cursor.firstBatch[3], {key: {"": 1}});
    assert.eq(cmdRes.cursor.firstBatch[4], {key: {"": 5}});
    assert.eq(cmdRes.cursor.firstBatch[5], {key: {"": 9}});

    st.stop();
})();
