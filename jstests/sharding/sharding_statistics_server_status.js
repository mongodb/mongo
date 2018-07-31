//
// Tests that serverStatus includes sharding statistics by default and the sharding statistics are
// indeed the correct values. Does not test the catalog cache portion of sharding statistics.
//

(function() {
    'use strict';
    function ShardStat() {
        this.countDonorMoveChunkStarted = 0;
        this.countRecipientMoveChunkStarted = 0;
        this.countDocsClonedOnRecipient = 0;
        this.countDocsClonedOnDonor = 0;
        this.countDocsDeletedOnDonor = 0;
    }

    function incrementStatsAndCheckServerShardStats(donor, recipient, numDocs) {
        ++donor.countDonorMoveChunkStarted;
        donor.countDocsClonedOnDonor += numDocs;
        ++recipient.countRecipientMoveChunkStarted;
        recipient.countDocsClonedOnRecipient += numDocs;
        donor.countDocsDeletedOnDonor += numDocs;
        const statsFromServerStatus = shardArr.map(function(shardVal) {
            return shardVal.getDB('admin').runCommand({serverStatus: 1}).shardingStatistics;
        });
        for (let i = 0; i < shardArr.length; ++i) {
            assert(statsFromServerStatus[i]);
            assert(statsFromServerStatus[i].countStaleConfigErrors);
            assert(statsFromServerStatus[i].totalCriticalSectionCommitTimeMillis);
            assert(statsFromServerStatus[i].totalCriticalSectionTimeMillis);
            assert(statsFromServerStatus[i].totalDonorChunkCloneTimeMillis);
            assert.eq(stats[i].countDonorMoveChunkStarted,
                      statsFromServerStatus[i].countDonorMoveChunkStarted);
            assert.eq(stats[i].countDocsClonedOnRecipient,
                      statsFromServerStatus[i].countDocsClonedOnRecipient);
            assert.eq(stats[i].countDocsClonedOnDonor,
                      statsFromServerStatus[i].countDocsClonedOnDonor);
            assert.eq(stats[i].countDocsDeletedOnDonor,
                      statsFromServerStatus[i].countDocsDeletedOnDonor);
            assert.eq(stats[i].countRecipientMoveChunkStarted,
                      statsFromServerStatus[i].countRecipientMoveChunkStarted);
        }
    }
    const st = new ShardingTest({shards: 2, mongos: 1});
    const mongos = st.s0;
    const admin = mongos.getDB("admin");
    const coll = mongos.getCollection("db.coll");
    const numDocsToInsert = 3;
    const shardArr = [st.shard0, st.shard1];
    const stats = [new ShardStat(), new ShardStat()];
    let numDocsInserted = 0;

    assert.commandWorked(admin.runCommand({enableSharding: coll.getDB() + ""}));
    st.ensurePrimaryShard(coll.getDB() + "", st.shard0.shardName);
    assert.commandWorked(admin.runCommand({shardCollection: coll + "", key: {_id: 1}}));
    assert.commandWorked(admin.runCommand({split: coll + "", middle: {_id: 0}}));

    // Move chunk from shard0 to shard1 without docs.
    assert.commandWorked(
        mongos.adminCommand({moveChunk: coll + '', find: {_id: 1}, to: st.shard1.shardName}));
    incrementStatsAndCheckServerShardStats(stats[0], stats[1], numDocsInserted);

    // Insert docs and then move chunk again from shard1 to shard0.
    for (let i = 0; i < numDocsToInsert; ++i) {
        assert.writeOK(coll.insert({_id: i}));
        ++numDocsInserted;
    }
    assert.commandWorked(mongos.adminCommand(
        {moveChunk: coll + '', find: {_id: 1}, to: st.shard0.shardName, _waitForDelete: true}));
    incrementStatsAndCheckServerShardStats(stats[1], stats[0], numDocsInserted);

    // Check that numbers are indeed cumulative. Move chunk from shard0 to shard1.
    assert.commandWorked(mongos.adminCommand(
        {moveChunk: coll + '', find: {_id: 1}, to: st.shard1.shardName, _waitForDelete: true}));
    incrementStatsAndCheckServerShardStats(stats[0], stats[1], numDocsInserted);

    // Move chunk from shard1 to shard0.
    assert.commandWorked(mongos.adminCommand(
        {moveChunk: coll + '', find: {_id: 1}, to: st.shard0.shardName, _waitForDelete: true}));
    incrementStatsAndCheckServerShardStats(stats[1], stats[0], numDocsInserted);

    st.stop();

})();
