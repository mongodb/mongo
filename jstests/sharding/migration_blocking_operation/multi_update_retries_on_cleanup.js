/*
 * Test that _shardsvrCoordinateMultiUpdate retries the update when
 * MigrationBlockingOperationCoordinator throws a cleanup error.
 * @tags: [
 *  requires_fcv_80
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: {rs0: {nodes: 3}}});
const dbName = "test";
const collName = "coll";
const namespace = `${dbName}.${collName}`;
const shard0Primary = st.rs0.getPrimary();

assert.commandWorked(
    st.s0
        .getDB(dbName)
        .getCollection(collName)
        .insertMany([
            {
                _id: 1,
                member: "abc123",
                points: 0,
            },
            {
                _id: 2,
                member: "abc123",
                points: 100,
            },
        ]),
);

function coordinateMultiUpdateShell() {
    return startParallelShell(
        funWithArgs(
            function (dbName, collName) {
                const databaseVersion = assert.commandWorked(db.adminCommand({getDatabaseVersion: dbName})).dbVersion;
                assert.commandWorked(
                    db.adminCommand({
                        _shardsvrCoordinateMultiUpdate: `${dbName}.${collName}`,
                        uuid: UUID(),
                        databaseVersion,
                        command: {
                            update: collName,
                            updates: [{q: {member: "abc123"}, u: {$set: {points: 50}}, multi: true}],
                        },
                    }),
                );
            },
            dbName,
            collName,
        ),
        shard0Primary.port,
    );
}

const coordinatorCleaningUpFailpoint = configureFailPoint(shard0Primary, "hangBeforeRemovingCoordinatorDocument");

const awaitMultiUpdate = coordinateMultiUpdateShell();
coordinatorCleaningUpFailpoint.wait();

const hangAfterFetchingCoordinator = configureFailPoint(
    shard0Primary,
    "hangAfterFetchingMigrationBlockingOperationCoordinator",
);

const awaitSecondMultiUpdate = coordinateMultiUpdateShell();
hangAfterFetchingCoordinator.wait();

const hitCleanupErrorFailpoint = configureFailPoint(shard0Primary, "hangAfterCatchingCleanupError");

hangAfterFetchingCoordinator.off();
coordinatorCleaningUpFailpoint.off();
awaitMultiUpdate();

hitCleanupErrorFailpoint.wait();
hitCleanupErrorFailpoint.off();

awaitSecondMultiUpdate();
st.stop();
