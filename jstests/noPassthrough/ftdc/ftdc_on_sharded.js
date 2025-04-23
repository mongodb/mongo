/**
 * Test that verifies FTDC diagnosticDataCollectionDirectorySizeMB on sharded.
 * @tags: [
 *   requires_sharding,
 * ]
 */
import {verifyCommonFTDCParameters} from "jstests/libs/ftdc.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

let testPath = MongoRunner.toRealPath('ftdc_setdir');

const st = new ShardingTest({
    shards: 1,
    mongos: {
        s0: {
            setParameter: {
                diagnosticDataCollectionDirectoryPath: testPath,
                diagnosticDataCollectionEnabled: 1,
            },
        },
    },
    nodes: 1,
    configShard: true,
});

jsTestLog("Verifying on mongos");
const mongosAdmin = st.s0.getDB('admin');
verifyCommonFTDCParameters(mongosAdmin, true);

jsTestLog("Verifying on mongod");
const mongodAdmin = st.shard0.getDB('admin');
verifyCommonFTDCParameters(mongodAdmin, true);
st.stop();
