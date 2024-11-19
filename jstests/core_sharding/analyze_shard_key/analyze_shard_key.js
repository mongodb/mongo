/**
 * Tests support for the analyzeShardKey command.
 *
 * @tags: [
 *   assumes_balancer_off,
 *   does_not_support_stepdowns,
 *   requires_fcv_70,
 * ]
 */
import {
    setParameterOpts,
    testExistingShardedCollection,
    testExistingUnshardedCollection,
    testNonExistingCollection
} from "jstests/sharding/analyze_shard_key/libs/analyze_shard_key_common_tests.js";

const shardNames = db.adminCommand({listShards: 1}).shards.map(shard => shard._id);

{
    const dbName = db.getName();
    const mongos = db.getMongo();
    assert.commandWorked(db.adminCommand({enableSharding: dbName}));
    const setParamCmd = Object.assign({setParameter: 1}, setParameterOpts);
    assert.commandWorked(db.adminCommand(setParamCmd));
    const testCases = [{conn: mongos, isSupported: true, isMongos: true}];
    testNonExistingCollection(dbName, testCases);
    testExistingUnshardedCollection(dbName, mongos, testCases);
    if (shardNames.length < 2) {
        print(jsTestName() +
              " testExistingShardedCollection will not run; at least 2 shards are required.");
    } else {
        testExistingShardedCollection(dbName, mongos, testCases);
    }
}
