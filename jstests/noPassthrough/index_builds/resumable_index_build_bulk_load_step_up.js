/**
 * Tests that an index build resumed in bulk load on a secondary can finish after stepping up between
 * its first side-writes drain and commit readiness.
 *
 * @tags: [
 *   requires_index_build_resumability,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
import {configureFailPoint, kDefaultWaitForFailPointTimeout} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ResumableIndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

describe("Step-up after resumed index bulk loading", function () {
    let rst;

    before(function () {
        rst = new ReplSetTest({nodes: 2});
        rst.startSet();
        rst.initiate();
    });

    after(function () {
        // Also release pauses if an assertion fails before the normal unpause sequence.
        for (const node of rst.nodes) {
            for (const failPoint of [
                "hangBeforeBuildingIndex",
                "hangDuringIndexBuildBulkLoadYield",
                "hangAfterIndexBuildFirstDrain",
            ]) {
                assert.commandWorked(
                    node.adminCommand({configureFailPoint: failPoint, mode: "off"}),
                );
            }
        }
        rst.stopSet();
    });

    it("completes a bulk-load resume after becoming primary before commit readiness", function () {
        const primary = rst.getPrimary();
        let secondary = rst.getSecondary();
        const dbName = "test";
        const coll = primary.getDB(dbName).getCollection(jsTestName());
        const indexName = "a_1";
        assert.commandWorked(coll.insert([{a: 1}, {a: 2}, {a: 3}], {writeConcern: {w: 2}}));

        // Keep the primary's build paused until the resumed secondary steps up.
        const primaryDrain = configureFailPoint(primary, "hangAfterIndexBuildFirstDrain");
        const secondaryBeforeBuild = configureFailPoint(secondary, "hangBeforeBuildingIndex");
        const awaitCreateIndex = ResumableIndexBuildTest.createIndexWithSideWrites(
            rst,
            function (collName, indexSpec, indexName) {
                assert.commandFailedWithCode(
                    db.getCollection(collName).createIndex(indexSpec, {name: indexName}),
                    ErrorCodes.InterruptedDueToReplStateChange,
                );
            },
            coll,
            {a: 1},
            indexName,
            [{a: 4}],
        );
        primaryDrain.wait();
        secondaryBeforeBuild.wait();

        let secondaryColl = secondary.getDB(dbName).getCollection(coll.getName());
        const buildUUIDs = ResumableIndexBuildTest.restart(
            rst,
            secondary,
            secondaryColl,
            [[indexName]],
            [{name: "hangIndexBuildDuringBulkLoadPhase", logIdWithIndexName: 4924400}],
            0,
            false /* shouldComplete */,
            undefined /* failPointAfterStartup */,
            undefined /* runBeforeStartup */,
            {
                setParameter: {
                    "failpoint.hangDuringIndexBuildBulkLoadYield": tojson({
                        mode: "alwaysOn",
                        data: {namespace: coll.getFullName()},
                    }),
                    "failpoint.hangAfterIndexBuildFirstDrain": tojson({mode: "alwaysOn"}),
                    // Force a yield after the final key.
                    internalIndexBuildBulkLoadYieldIterations: 1,
                },
            },
        );

        // Step-up requires the fixture's current connection after restart.
        secondary = rst.getSecondary();
        secondaryColl = secondary.getDB(dbName).getCollection(coll.getName());

        // Wait for SECONDARY with locks yielded, before restoring the collection..
        assert.commandWorked(
            secondary.adminCommand({
                waitForFailPoint: "hangDuringIndexBuildBulkLoadYield",
                timesEntered: 1,
                maxTimeMS: kDefaultWaitForFailPointTimeout,
            }),
        );
        rst.awaitSecondaryNodes();
        assert.commandWorked(
            secondary.adminCommand({
                configureFailPoint: "hangDuringIndexBuildBulkLoadYield",
                mode: "off",
            }),
        );

        assert.commandWorked(
            secondary.adminCommand({
                waitForFailPoint: "hangAfterIndexBuildFirstDrain",
                timesEntered: 1,
                maxTimeMS: kDefaultWaitForFailPointTimeout,
            }),
        );
        rst.stepUp(secondary);
        assert.commandWorked(
            secondary.adminCommand({
                configureFailPoint: "hangAfterIndexBuildFirstDrain",
                mode: "off",
            }),
        );
        primaryDrain.off();

        ResumableIndexBuildTest.assertCompleted(secondary, secondaryColl, buildUUIDs, [indexName]);
        awaitCreateIndex();
        ResumableIndexBuildTest.checkResume(
            secondary,
            buildUUIDs,
            ["bulk load"],
            [{skippedPhaseLogID: 20391}],
        );
        ResumableIndexBuildTest.checkIndexes(rst, dbName, coll.getName(), [[indexName]], [{a: 5}]);
    });
});
