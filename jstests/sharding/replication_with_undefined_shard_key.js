// Test for SERVER-31953 where secondaries crash when replicating an oplog entry where the document
// identifier in the oplog entry contains a shard key value that contains an undefined value.
(function() {
    "use strict";

    const st = new ShardingTest({merizos: 1, config: 1, shard: 1, rs: {nodes: 2}});
    const merizosDB = st.s.getDB("test");
    const merizosColl = merizosDB.mycoll;

    // Shard the test collection on the "x" field.
    assert.commandWorked(merizosDB.adminCommand({enableSharding: merizosDB.getName()}));
    assert.commandWorked(merizosDB.adminCommand({
        shardCollection: merizosColl.getFullName(),
        key: {x: 1},
    }));

    // Insert a document with a literal undefined value.
    assert.writeOK(merizosColl.insert({x: undefined}));

    jsTestLog("Doing writes that generate oplog entries including undefined document key");

    assert.writeOK(merizosColl.update(
        {},
        {$set: {a: 1}},
        {multi: true, writeConcern: {w: 2, wtimeout: ReplSetTest.kDefaultTimeoutMs}}));
    assert.writeOK(
        merizosColl.remove({}, {writeConcern: {w: 2, wtimeout: ReplSetTest.kDefaultTimeoutMs}}));

    st.stop();
})();