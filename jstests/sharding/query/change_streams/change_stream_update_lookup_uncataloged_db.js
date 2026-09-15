/**
 * Tests that an updateLookup change stream opened directly against a shard mongoD, watching a
 * database that was created directly on the shard (bypassing mongos DDL, and therefore missing a
 * 'config.databases' entry), still returns the real post-image on a filter-only '$set' update.
 *
 * The fix (LocalLookupEligibilityFactoryImpl) is client-origin based, not namespace based: a
 * client that connects directly to a shard rather than through mongos is treated as
 * replica-set-like for lookup purposes (see mongod_process_interface_factory.cpp for the same
 * distinction on the legacy aggregation-fallback path), so this test's uncataloged-database setup
 * is only a means of reproducing an unregistered namespace, not the fix's actual trigger.
 *
 * @tags: [
 *   uses_change_streams,
 * ]
 */
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    ChangeStreamTest,
    ChangeStreamWatchMode,
    withChangeStreamTest,
} from "jstests/libs/query/change_stream_util.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";
import {
    assertCreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";

describe("change stream updateLookup on an uncataloged database", function () {
    let st;
    let shard0Db;
    let shard0Coll;
    const dbName = "__mdb_internal_search";
    const collName = "mv_coll";

    before(function () {
        st = new ShardingTest({
            shards: 1,
            mongos: 1,
            rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}},
        });

        // Database is created directly on the shard below, without going through mongos. This
        // mirrors how mongot materializes '__mdb_internal_search' directly on the shard it is
        // attached to: the database exists in the shard's local catalog but has no entry in
        // 'config.databases'.
        shard0Db = st.rs0.getPrimary().getDB(dbName);
    });

    after(function () {
        st.stop();
    });

    beforeEach(function () {
        shard0Coll = assertCreateCollection(shard0Db, collName);
        assert.commandWorked(shard0Coll.insert({_id: 1, marker: "seed"}));

        assert.eq(
            null,
            st.s.getDB("config").databases.findOne({_id: dbName}),
            "expected the database to be absent from config.databases",
        );
    });

    afterEach(function () {
        assertDropCollection(shard0Db, collName);
    });

    for (const optimizedUpdateLookup of [true, false]) {
        for (const watchMode of Object.values(ChangeStreamWatchMode)) {
            it(`returns the real post-image for insert and filter-only $set update events (featureFlagChangeStreamOptimizedUpdateLookup=${optimizedUpdateLookup})`, function () {
                // Cluster-level streams must be opened against the admin database; the others
                // against the __mdb_internal_search database itself. Both live on the same direct
                // shard connection.
                runWithParamsAllNonConfigNodes(
                    st.s.getDB("admin"),
                    {featureFlagChangeStreamOptimizedUpdateLookup: optimizedUpdateLookup},
                    () => {
                        const csDb = ChangeStreamTest.getDBForChangeStream(watchMode, shard0Db);
                        withChangeStreamTest(csDb, (cst) => {
                            const cursor = cst.getChangeStream({
                                watchMode,
                                coll: shard0Coll,
                                options: {fullDocument: "updateLookup"},
                            });

                            assert.commandWorked(
                                shard0Coll.insert({_id: 2, marker: "insert-probe"}),
                            );

                            // A filter-only update, mirroring mongot's MaterializedViewWriter path
                            // for updates that only change a filter field: applied as a bare '$set',
                            // not a full-document replace.
                            assert.commandWorked(
                                shard0Coll.update({_id: 1}, {$set: {marker: "updated"}}),
                            );

                            cst.assertNextChangesEqual({
                                cursor,
                                expectedChanges: [
                                    {
                                        operationType: "insert",
                                        ns: {db: dbName, coll: collName},
                                        documentKey: {_id: 2},
                                        fullDocument: {_id: 2, marker: "insert-probe"},
                                    },
                                    {
                                        operationType: "update",
                                        ns: {db: dbName, coll: collName},
                                        documentKey: {_id: 1},
                                        fullDocument: {_id: 1, marker: "updated"},
                                    },
                                ],
                            });
                        });
                    },
                );
            });
        }
    }
});
