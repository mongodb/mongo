import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

// TODO (SERVER-98118): remove once 9.0 becomes last LTS.
// Asserts no config.cache.* collections exist on shards when the authoritative shard catalog is in use.
export function assertNoLegacyShardCacheCollections(db) {
    if (!FeatureFlagUtil.isPresentAndEnabled(db, "AuthoritativeShardsCRUD")) {
        return;
    }

    const listShardsRes = db.adminCommand({listShards: 1});
    if (listShardsRes.code === ErrorCodes.CommandNotFound) {
        return;
    }
    assert.commandWorked(listShardsRes);

    for (const shard of listShardsRes.shards) {
        const shardConn = new Mongo(shard.host);
        try {
            const cacheColls = shardConn.getDB("config").getCollectionInfos({name: /^cache\./});
            assert.eq(
                cacheColls.length,
                0,
                "Expected no config.cache.* collections on shard with authoritative shard catalog enabled",
                {shardId: shard._id, cacheColls},
            );
        } finally {
            shardConn.close();
        }
    }
}
