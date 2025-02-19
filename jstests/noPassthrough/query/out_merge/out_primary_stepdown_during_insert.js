/**
 * Tests the behavior of $out on a replica set and a sharded cluster if the replica set primary
 * steps down between batches of inserts into the temp collection. The temp collection exists on the
 * primary and is deleted during step down which can result in incomplete $out results being
 * returned to the user. This test asserts that an error is thrown.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const sourceCollName = "sourceColl";

function insertDocuments(coll) {
    // Insert 20 ~1MB documents so they don"t all fit in one 16MB out write batch.
    let largeVal = "a".repeat(1024 * 1024);
    for (let i = 0; i < 20; i++) {
        assert.commandWorked(coll.insert({x: i, a: largeVal, t: ISODate()}));
    }
}

function setUpFn(db) {
    assert.commandWorked(db.dropDatabase());
    insertDocuments(db[sourceCollName]);
}

function runTest(connInfo) {
    const db = connInfo.conn.getDB("test");
    setUpFn(db);
    connInfo.awaitReplication();

    for (const failpoint
             of ["hangWhileBuildingDocumentSourceOutBatch", "hangDollarOutAfterInsert"]) {
        // Test both general $out and $out to a timeseries collection, with 'apiStrict' true and
        // false.
        for (const isTimeseries of [false, true]) {
            for (const useAPIStrict of [false, true]) {
                jsTestLog("Testing " + (isTimeseries ? " timeseries " : " normal ") +
                          " collection with apiStrict set to " + useAPIStrict +
                          " and with failpoint " + failpoint + " enabled");

                let cmdObj = {
                    aggregate: sourceCollName,
                    pipeline: [{$out: {db: "test", coll: "out"}}],
                    cursor: {},
                    $readPreference: {mode: "secondary"},
                };
                if (isTimeseries) {
                    cmdObj.pipeline[0].$out.timeseries = {timeField: "t"};
                }

                if (useAPIStrict) {
                    cmdObj.apiStrict = true;
                    cmdObj.apiVersion = "1";
                }

                let fp = configureFailPoint(connInfo.getSecondary(), failpoint);

                // Start $out. Make it hang after writing the first batch (which consists of one
                // single document).
                const awaitAgg = startParallelShell(
                    funWithArgs((cmdObj) => {
                        let sourceDB = db.getSiblingDB("test");
                        let result = sourceDB.runCommand(cmdObj);
                        // The command can fail with different codes based on
                        // whether we"re doing $out to timeseries, on a sharded
                        // cluster, etc.
                        assert.commandFailedWithCode(result, [
                            ErrorCodes.CollectionUUIDMismatch,
                            8555700 /* Thrown when catalog changes are detected in timeseries */,
                            ErrorCodes.NamespaceNotFound
                        ]);
                    }, cmdObj), connInfo.getNodeRunningOut().port);

                fp.wait();

                // Stepdown the primary.
                let initialPrimary = connInfo.getPrimary();
                assert.commandWorked(
                    initialPrimary.adminCommand({replSetStepDown: 60, force: true}));

                // Wait for a new primary to be elected.
                const newPrimary = connInfo.getPrimary();
                assert.neq(newPrimary.port, initialPrimary.port);

                fp.off();
                awaitAgg();
            }
        }
    }
}

jsTestLog("Testing against a replica set");
var rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate(null, null, {initiateWithDefaultElectionTimeout: true});
const replSetConn = new Mongo(rst.getURL());
let connInfo = {
    conn: replSetConn,
    getPrimary: () => rst.getPrimary(),
    getSecondary: () => rst.getSecondary(),
    getNodeRunningOut: () => rst.getSecondary(),
    awaitReplication: () => rst.awaitReplication()
};
runTest(connInfo);
rst.stopSet();

jsTestLog("Testing against a sharded cluster");
const st = new ShardingTest({shards: 1, rs: {nodes: 2}, initiateWithDefaultElectionTimeout: true});
connInfo = {
    conn: st.s,
    getPrimary: () => st.rs0.getPrimary(),
    getSecondary: () => st.rs0.getSecondary(),
    getNodeRunningOut: () => st.s,
    awaitReplication: () => st.awaitReplicationOnShards()
};
runTest(connInfo);
st.stop();
