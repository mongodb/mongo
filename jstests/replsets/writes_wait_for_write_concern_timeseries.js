/**
 * Tests that commands that accept write concern correctly return write concern errors when run
 * through mongos on timeseries views.
 *
 * @tags: [
 * multiversion_incompatible,
 * uses_transactions,
 * does_not_support_stepdowns,
 * ]
 */

import {
    checkWriteConcernBehaviorAdditionalCRUDOps,
    checkWriteConcernBehaviorForAllCommands
} from "jstests/libs/write_concern_all_commands.js";

const name = jsTestName();
const replTest = new ReplSetTest({
    name: name,
    nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
});
replTest.startSet();
replTest.initiate();

const preSetupTimeseries = function(conn, cluster, dbName, collName) {
    let db = conn.getDB(dbName);
    assert.commandWorked(
        db.createCollection(collName, {timeseries: {timeField: "time", metaField: "meta"}}));
};

checkWriteConcernBehaviorForAllCommands(replTest.getPrimary(),
                                        replTest,
                                        "rs" /* clusterType */,
                                        preSetupTimeseries,
                                        false /* shardedCollection */,
                                        true /*limitToTimeseriesViews*/);
checkWriteConcernBehaviorAdditionalCRUDOps(replTest.getPrimary(),
                                           replTest,
                                           "rs" /* clusterType */,
                                           preSetupTimeseries,
                                           false /* shardedCollection */,
                                           false /* writeWithoutShardKey */,
                                           true /*limitToTimeseriesViews*/);

replTest.stopSet();
