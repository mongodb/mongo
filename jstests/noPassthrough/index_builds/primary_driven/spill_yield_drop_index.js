/**
 * Tests dropping the index being built while a primary-driven index build has released its locks at
 * a sorter spill batch boundary.
 *
 * The container-based spiller yields between the batches of a single spill (see the onSpillBatch
 * callback in multi_index_block.cpp). While yielded the collection is unlocked, so the index being
 * built can be dropped out from under the build and restoring the transaction resources afterwards
 * can fail. The build must abort cleanly and the server must survive.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {after, describe, it} from "jstests/libs/mochalite.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const dbName = jsTestName();
const primaryDB = primary.getDB(dbName);

const requiredFlags = ["PrimaryDrivenIndexBuilds", "ContainerWrites"];
for (const flag of requiredFlags) {
    if (!FeatureFlagUtil.isPresentAndEnabled(primaryDB, flag)) {
        jsTest.log.info("Skipping: required feature flag is disabled", {flag});
        rst.stopSet();
        quit();
    }
}

describe("dropping an index while its build is yielded mid-spill", function () {
    after(function () {
        rst.stopSet();
    });

    it("aborts the build cleanly", function () {
        // A 1 MB sorter budget makes the build spill after a handful of the 64 KB keys below, and a
        // batch size of one key makes every key of that spill a batch boundary, so the spill yield
        // is reached as soon as the first spill starts. A large yield period disables time-based
        // collection scan yields so the spill yield is the first place the build releases its locks.
        assert.commandWorked(
            primary.adminCommand({
                setParameter: 1,
                maxIndexBuildMemoryUsageMegabytes: 1,
                primaryDrivenIndexBuildSorterInsertionBatchSize: 1,
                internalQueryExecYieldIterations: 1000,
                internalQueryExecYieldPeriodMS: 60000,
            }),
        );

        const coll = primaryDB.getCollection("coll");
        const bulk = coll.initializeUnorderedBulkOp();
        for (let i = 0; i < 60; i++) {
            bulk.insert({_id: i, a: "x".repeat(64 * 1024)});
        }
        assert.commandWorked(bulk.execute());

        const indexName = "a_1";
        const spillYieldFp = configureFailPoint(primary, "hangDuringIndexBuildSpillYield");

        const awaitIndexBuild = IndexBuildTest.startIndexBuild(
            primary,
            coll.getFullName(),
            {a: 1},
            {name: indexName},
            [ErrorCodes.IndexBuildAborted],
        );

        jsTest.log.info("Waiting for the index build to yield mid-spill");
        spillYieldFp.wait();

        jsTest.log.info("Dropping the index while the build holds no locks");
        const awaitDrop = startParallelShell(
            funWithArgs(
                function (dbName, collName, indexName) {
                    const coll = db.getSiblingDB(dbName).getCollection(collName);
                    assert.commandWorked(coll.dropIndex(indexName));
                },
                dbName,
                coll.getName(),
                indexName,
            ),
            primary.port,
        );

        // "Cleaned up index build after abort". Waiting for this guarantees the drop has aborted the
        // build before we let it try to restore its locks and resume spilling.
        checkLog.containsJson(primary, 465611);
        spillYieldFp.off();

        awaitIndexBuild();
        awaitDrop();

        assert.commandWorked(primary.adminCommand({ping: 1}));
        IndexBuildTest.assertIndexesSoon(coll, 1, ["_id_"]);
        assert(coll.drop());
    });
});
