
/**
 * Reproduce issue in CheckRoutingTableConsistency with resharding described in SERVER-84384.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

const st = new ShardingTest({shards: 2});

const dbName = jsTestName();
const collName = "testColl";
const collNss = dbName + "." + collName;

assert.commandWorked(st.s.adminCommand({shardCollection: collNss, key: {x: 1}}));

const db = st.s.getDB(dbName);
const configPrimary = st.configRS.getPrimary();

assert.commandWorked(db[collName].insertOne({x: 1, y: 2}));

const fp = configureFailPoint(configPrimary, "reshardingPauseCoordinatorBeforeBlockingWrites");

const joinReshard = startParallelShell(
    funWithArgs(function(collNss) {
        assert.commandWorked(db.adminCommand({reshardCollection: collNss, key: {y: 1}}));
    }, collNss), st.s.port);

fp.wait();

// Now we are in the middle of a resharding operation, checkHistoricalPlacementMetadataConsistency
// will fail if it considers system.resharding.* collections.
st.checkRoutingTableConsistency();

fp.off();
joinReshard();

st.stop();
