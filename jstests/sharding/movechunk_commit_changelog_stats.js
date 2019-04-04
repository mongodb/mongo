//
// Tests that the changelog entry for moveChunk.commit contains stats on the migration.
//

(function() {
    'use strict';

    var st = new ShardingTest({merizos: 1, shards: 2});
    var kDbName = 'db';

    var merizos = st.s0;
    var shard0 = st.shard0.shardName;
    var shard1 = st.shard1.shardName;

    assert.commandWorked(merizos.adminCommand({enableSharding: kDbName}));
    st.ensurePrimaryShard(kDbName, shard0);

    function assertCountsInChangelog() {
        let changeLog = st.s.getDB('config').changelog.find({what: 'moveChunk.commit'}).toArray();
        assert.gt(changeLog.length, 0);
        for (let i = 0; i < changeLog.length; i++) {
            assert(changeLog[i].details.hasOwnProperty('counts'));
        }
    }

    var ns = kDbName + '.fooHashed';
    assert.commandWorked(merizos.adminCommand({shardCollection: ns, key: {_id: 'hashed'}}));

    var aChunk = merizos.getDB('config').chunks.findOne({_id: RegExp(ns), shard: shard0});
    assert(aChunk);

    // Assert counts field exists in the changelog entry for moveChunk.commit
    assert.commandWorked(
        merizos.adminCommand({moveChunk: ns, bounds: [aChunk.min, aChunk.max], to: shard1}));
    assertCountsInChangelog();

    merizos.getDB(kDbName).fooHashed.drop();

    st.stop();
})();