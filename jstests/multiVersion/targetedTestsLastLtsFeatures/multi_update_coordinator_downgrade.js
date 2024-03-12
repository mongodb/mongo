/**
 * Tests that any running MultiUpdateCoordinators are aborted and properly
 * cleaned up before downgrading to 7.0.
 *
 *  @tags: [
 *      featureFlagPauseMigrationsDuringMultiUpdatesAvailable
 *  ]
 */

import {migrationsAreAllowed} from "jstests/libs/chunk_manipulation_util.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";

const dbName = "test";
const collName = "coll";
const namespace = `${dbName}.${collName}`;

function runDowngradeTest(phase) {
    jsTestLog(`Running MultiUpdateCoordinator downgrade test from phase ${phase}`);
    const st = new ShardingTest();
    const mongos = st.s0;

    const sourceCollection = mongos.getCollection(namespace);
    CreateShardedCollectionUtil.shardCollectionWithChunks(sourceCollection, {key: 1}, [
        {min: {key: MinKey}, max: {key: MaxKey}, shard: st.shard0.shardName},
    ]);
    sourceCollection.insertMany([{key: 1, counter: 0}, {key: 2, counter: 0}]);
    const dbPrimaryRsPrimary = st.getPrimaryShard(dbName).rs.getPrimary();

    function downgradeFcvFromParallelShell() {
        return startParallelShell(
            () => {assert.commandWorked(
                db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}))},
            st.s.port);
    }

    function coordinateMultiUpdateFromParallelShell() {
        return startParallelShell(
            funWithArgs((nss, collName) => {
                db.adminCommand({
                    _shardsvrCoordinateMultiUpdate: nss,
                    uuid: UUID(),
                    command: {
                        update: collName,
                        updates: [{q: {counter: 0}, u: {$set: {counter: 1}}, multi: true}]
                    }
                });
            }, namespace, collName), dbPrimaryRsPrimary.port);
    }

    function assertNoDocumentsIn(dbName, collName) {
        const docs = dbPrimaryRsPrimary.getDB(dbName).getCollection(collName).find({}).toArray();
        assert.eq(docs, []);
    }

    const phaseFp = configureFailPoint(dbPrimaryRsPrimary,
                                       "pauseDuringMultiUpdateCoordinatorPhaseTransition",
                                       {progress: "after", phase});
    const abortFp =
        configureFailPoint(dbPrimaryRsPrimary, "hangAfterAbortingMultiUpdateCoordinators");

    const joinUpdate = coordinateMultiUpdateFromParallelShell();
    phaseFp.wait();
    const joinDowngrade = downgradeFcvFromParallelShell();
    abortFp.wait();
    abortFp.off();
    phaseFp.off();
    joinDowngrade();

    assertNoDocumentsIn("config", "system.sharding_ddl_coordinators");
    assertNoDocumentsIn("config", "localMigrationBlockingOperations.multiUpdateCoordinators");
    assert(migrationsAreAllowed(st.s.getDB(dbName), collName));

    joinUpdate();
    st.stop();
}

const phases = ["acquireSession", "blockMigrations", "performUpdate", "success", "done"];
for (const phase of phases) {
    runDowngradeTest(phase);
}
