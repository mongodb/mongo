// Tests that a stale merizos would route writes correctly to the right shard after
// an unsharded collection was moved to another shard.
(function() {
    "use strict";

    const st = new ShardingTest({
        shards: 2,
        merizos: 2,
        rs: {
            nodes: 1,
        },
    });

    const testName = 'test';
    const merizosDB = st.s0.getDB(testName);

    // Ensure that shard1 is the primary shard.
    assert.commandWorked(merizosDB.adminCommand({enableSharding: merizosDB.getName()}));
    st.ensurePrimaryShard(merizosDB.getName(), st.rs1.getURL());

    // Before moving the collection, issue a write through merizos2 to make it aware
    // about the location of the collection before the move.
    const merizos2DB = st.s1.getDB(testName);
    const merizos2Coll = merizos2DB[testName];
    assert.writeOK(merizos2Coll.insert({_id: 0, a: 0}));

    st.ensurePrimaryShard(merizosDB.getName(), st.rs0.getURL());

    assert.writeOK(merizos2Coll.insert({_id: 1, a: 0}));

    st.stop();
})();
