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
    testExistingShardedCollection,
    testExistingUnshardedCollection,
    testNonExistingCollection
} from "jstests/sharding/analyze_shard_key/libs/analyze_shard_key_common_tests.js";

const shardNames = db.adminCommand({listShards: 1}).shards.map(shard => shard._id);

const setParameterOpts = {
    analyzeShardKeyNumRanges: 100
};
// The sampling-based initial split policy needs 10 samples per split point so
// 10 * analyzeShardKeyNumRanges is the minimum number of distinct shard key values that the
// collection must have for the command to not fail to generate split points.
const numDocs = 10 * setParameterOpts.analyzeShardKeyNumRanges;

{
    const dbName = db.getName();
    const mongos = db.getMongo();
    assert.commandWorked(db.adminCommand({enableSharding: dbName}));
    const setParamCmd = Object.assign({setParameter: 1}, setParameterOpts);
    assert.commandWorked(db.adminCommand(setParamCmd));
    const testCases = [{conn: mongos, isSupported: true, isMongos: true}];
    testNonExistingCollection(dbName, testCases);
    testExistingUnshardedCollection(dbName, mongos, testCases, numDocs);
    if (shardNames.length < 2) {
        print(jsTestName() +
              " testExistingShardedCollection will not run; at least 2 shards are required.");
    } else {
        testExistingShardedCollection(dbName, mongos, testCases, numDocs);
    }
}
