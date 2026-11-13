/**
 * Tests that an active query analyzer configuration is still in effect after the set restarts.
 *
 * This is a regression test for SERVER-136642. The QueryAnalysisCoordinator caches
 * config.queryAnalyzers in memory and bulk-loads it exactly once. In DSC, the replica set aware
 * service startup calls are made before the collection catalog has been loaded from the checkpoint,
 * so this load would return empty, even if there was a majority-committed config. SERVER-136642
 * moved this load from onStartup to onConsistentDataAvailable to make sure the hook does not run
 * until the collection catalog has been loaded from the checkpoint.
 *
 * @tags: [
 *   requires_fcv_70,
 *   requires_persistence,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const dbName = "testDb";
const collName = "testColl";
const ns = dbName + "." + collName;
const samplesPerSecond = 5;

describe("an active query analyzer configuration", function () {
    before(function () {
        this.rst = new ReplSetTest({
            nodes: 2,
            // Keep the node from refreshing its own configuration in the background, so that this
            // test is the only _refreshQueryAnalyzerConfiguration caller.
            nodeOptions: {
                setParameter: {
                    "failpoint.disableQueryAnalysisSampler": tojson({mode: "alwaysOn"}),
                },
            },
        });
        this.rst.startSet();
        this.rst.initiate();
    });

    after(function () {
        this.rst.stopSet();
    });

    it("is still served by the coordinator after a restart", function () {
        let primary = this.rst.getPrimary();
        assert.commandWorked(primary.getDB(dbName).createCollection(collName));
        const configureRes = assert.commandWorked(
            primary.adminCommand({
                configureQueryAnalyzer: ns,
                mode: "full",
                samplesPerSecond: samplesPerSecond,
            }),
        );

        // Confirm configureQueryAnalyzer waits for its config.queryAnalyzers write to be majority
        // committed before replying:
        // Confirm the optime the command waited on is at or behind the majority commit point.
        const status = assert.commandWorked(primary.adminCommand({replSetGetStatus: 1}));
        assert.lte(
            timestampCmp(configureRes.operationTime, status.optimes.lastCommittedOpTime.ts),
            0,
            "the configureQueryAnalyzer write is not majority committed",
            {operationTime: configureRes.operationTime, optimes: status.optimes},
        );

        // Run a majority read concern find t confirm it returns the document.
        const majorityRead = assert.commandWorked(
            primary.getDB("config").runCommand({
                find: "queryAnalyzers",
                filter: {_id: ns},
                readConcern: {level: "majority"},
            }),
        );
        assert.eq(
            1,
            majorityRead.cursor.firstBatch.length,
            "the config.queryAnalyzers document is not majority committed",
            {majorityRead},
        );

        // Restart the set.
        this.rst.stopSet(null /* signal */, true /* forRestart */);
        this.rst.startSet({restart: true});
        primary = this.rst.getPrimary();

        // Find the document to confirm the majority-committed configuration document outlives the restart.
        assert.neq(
            null,
            primary.getCollection("config.queryAnalyzers").findOne({_id: ns}),
            "the config.queryAnalyzers document did not survive the restart",
        );

        // Verify that the refresh succeeds and the config is correctly cached.
        const res = assert.commandWorked(
            primary.adminCommand({
                _refreshQueryAnalyzerConfiguration: 1,
                name: primary.host,
                numQueriesExecutedPerSecond: 1,
            }),
        );

        assert.eq(1, res.configurations.length, "configurations missing", {res});
        assert.eq(ns, res.configurations[0].ns, "unexpected namespace", {res});

        assert.commandWorked(primary.adminCommand({configureQueryAnalyzer: ns, mode: "off"}));
    });
});
