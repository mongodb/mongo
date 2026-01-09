/**
 * Basic checks on the consistency of the 'listShards' and 'getShardMap' command responses.
 */

(() => {
    // Checking shard list or shard mapping when have random shard adds and removes doesn't make
    // sense
    if (TestData.hasRandomShardsAddedRemoved) {
        return;
    }
    // Wait until getShardMap returns a non-empty map.
    // The command is based on the internal shard registry which relies on topology time gossipping
    // that might cause transient empty responses or incomplete onces.
    // Retry until the information is eventually propagated and consistent.
    assert.soon(() => {
        const listShardsResponse = assert.commandWorked(db.adminCommand({listShards: 1}));
        const getShardMapResponse = assert.commandWorked(db.adminCommand({getShardMap: 1}));

        jsTest.log("Check that getShardMap returns consistent information across its sections...");
        // NOTE: The config server is always included using 'config' as its shard ID, even when it
        // doesn't act as a regular shard.
        assert.eq(getShardMapResponse.map.length, getShardMapResponse.hosts.length);
        assert.eq(getShardMapResponse.map.length, getShardMapResponse.connStrings.length);
        for (const shardId in getShardMapResponse.map) {
            const replSetConnString = getShardMapResponse.map[shardId];
            assert.eq(shardId, getShardMapResponse.connStrings[replSetConnString]);
            const replSetHostList = replSetConnString.slice(replSetConnString.indexOf("/") + 1);
            for (let replSetHost of replSetHostList.split(",")) {
                assert.eq(shardId, getShardMapResponse.hosts[replSetHost]);
            }
        }

        jsTest.log("Check that listShards returns information consistent with getShardMap...");
        // In this case, 'config' only appears when a config shard is present.
        const numEntriesInGetShardMapResponse = Object.keys(getShardMapResponse.map).length;
        if (
            numEntriesInGetShardMapResponse !== listShardsResponse.shards.length + 1 &&
            numEntriesInGetShardMapResponse !== listShardsResponse.shards.length
        ) {
            return false;
        }

        // In case a shard is not found, retry after a small delay to allow for topology propagation.
        for (let shardInfo of listShardsResponse.shards) {
            if (shardInfo.host !== getShardMapResponse.map[shardInfo._id]) {
                return false;
            }
        }
        return true;
    });
})();
