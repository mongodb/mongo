/**
 * Smoke tests for $changeStream v2, ignoreRemovedShards mode in a sharded cluster.
 *
 * @tags: [
 *   # Assume balancer is off, and we do not get random moveChunk events during the test.
 *   assumes_balancer_off,
 *   featureFlagChangeStreamPreciseShardTargeting,
 *   requires_sharding,
 *   uses_change_streams,
 *   # Incompatible with embedded config server, because the config server would be embedded on a
 *   # shard that this test will remove.
 *   config_shard_incompatible,
 *   # The test runs for a long time, so do not run it on very slow build variants.
 *   incompatible_aubsan,
 *   tsan_incompatible,
 *   resource_intensive,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {describe, it, before, after, beforeEach, afterEach} from "jstests/libs/mochalite.js";
import {assertCreateCollection, assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {
    ChangeStreamTest,
    distributeCollectionDataOverShards,
    getClusterTime,
} from "jstests/libs/query/change_stream_util.js";
import {removeShard} from "jstests/sharding/libs/remove_shard_util.js";

describe("$changeStream v2, ignoreRemovedShards mode", function () {
    let st;
    let db;
    let coll;
    let csTest;
    let shardsAdded = [];

    let skipCheckingIndexesConsistentAcrossClusterWas;

    before(() => {
        // Store the current value of 'skipCheckingIndexesConsistentAcrossCluster' so that we can
        // restore it later.
        skipCheckingIndexesConsistentAcrossClusterWas = TestData.skipCheckingIndexesConsistentAcrossCluster;

        // Temporarily turn off this check because it fails when at least one of the shards of the
        // `ShardingTest` Fixture has been removed. It is fine to skip the index consistency in this
        // test because it focuses on change stream results correctness.
        TestData.skipCheckingIndexesConsistentAcrossCluster = true;
    });

    after(() => {
        // Restore the previous value of 'skipCheckingIndexesConsistentAcrossCluster' so that the
        // change does not leak into any following tests running in the same instance.
        TestData.skipCheckingIndexesConsistentAcrossCluster = skipCheckingIndexesConsistentAcrossClusterWas;
    });

    beforeEach(function () {
        // Create a sharded cluster with 4 shards.
        // Documents are only inserted on shard0, shard1, and shard2.
        // shard3 is only used so that we can remove shard0, shard1, and shard2 from the test later.
        // One shard needs to remain present so that we can successfully shut down the test later.
        st = new ShardingTest({
            shards: 4,
            mongos: 1,
            config: 1,
            rs: {
                nodes: 1,
                setParameter: {
                    writePeriodicNoops: true,
                    periodicNoopIntervalSecs: 1,
                },
            },
            other: {
                enableBalancer: false,
            },
        });

        db = st.s.getDB(jsTestName());
        db.dropDatabase();
        coll = db.test;
        shardsAdded = [];
    });

    afterEach(function () {
        if (csTest) {
            csTest.cleanUp();
            csTest = null;
        }
        db.dropDatabase();
        shardsAdded.forEach((shard) => {
            shard.stopSet();
        });
        st.stop();
        shardsAdded = [];
    });

    // Start the balancer.
    function startBalancer() {
        jsTest.log.info("Starting balancer");
        assert.commandWorked(st.s.adminCommand({balancerStart: 1}));
        jsTest.log.info("Balancer successfully started");
    }

    // Stop the balancer.
    function stopBalancer() {
        jsTest.log.info("Stopping balancer");
        assert.commandWorked(st.s.adminCommand({balancerStop: 1}));
        jsTest.log.info("Balancer successfully stopped");
    }

    // Start the balancer, execute the callback and stop the balancer again.
    function withBalancerEnabled(cb) {
        startBalancer();
        try {
            return cb();
        } finally {
            stopBalancer();
        }
    }

    // Query the current data distribution of the collection across the shards.
    function getCollDataDistribution() {
        let docs = {};
        [st.shard0, st.shard1, st.shard2, st.shard3].forEach((shardConn) => {
            docs[shardConn.shardName] = shardConn.getDB(db.getName())[coll.getName()].find().itcount();
        });
        shardsAdded.forEach((shard) => {
            docs[shard.shardName] = shard.getPrimary().getDB(db.getName())[coll.getName()].find().itcount();
        });
        return docs;
    }

    // Asserts that the collection data distribution matches the expected counts per shard.
    function assertCollDataDistribution(expectedCounts) {
        assert.soon(
            () => {
                const actual = getCollDataDistribution();
                return Object.keys(expectedCounts).every((shardName) => {
                    return actual[shardName] == expectedCounts[shardName];
                });
            },
            () => {
                return (
                    "Data distribution did not match expected counts. Current distribution: " +
                    tojsononeline(getCollDataDistribution()) +
                    ", expected: " +
                    tojsononeline(expectedCounts)
                );
            },
        );
    }

    // Builds an expected change event for an insert operation. The document key used is identical
    // to 'doc', unless the document key is explicitly specified in 'documentKey'.
    function buildExpectedInsertEvent(shard, doc, documentKey = undefined) {
        return {
            documentKey: documentKey ?? doc,
            fullDocument: Object.assign({}, doc, {shardId: shard.shardName}),
            ns: {db: db.getName(), coll: coll.getName()},
            operationType: "insert",
        };
    }

    // Builds an expected change event for the specified operation type plus the namespace of the
    // underlying collection.
    function buildExpectedEventForNamespace(operationType) {
        return {
            ns: {db: db.getName(), coll: coll.getName()},
            operationType,
        };
    }

    // Builds an expected change event for the specified operation type, without a namespace.
    function buildExpectedEventWithoutNamespace(operationType) {
        return {
            operationType,
        };
    }

    // Insert a document into the collection and verify that it was inserted on the expected shard.
    function insertDocumentOnShard(doc, shard) {
        let counts = getCollDataDistribution();
        doc.shardId = shard.shardName;
        assert.commandWorked(coll.insert(doc));
        counts[shard.shardName] += 1;
        assertCollDataDistribution(counts);
    }

    // Execute a series of operations on the collection, then decommission the specified shards.
    function executeCollectionOperations(shardsToDecommission, beforeDecommission, dropCollection) {
        // Create and shard a collection and allocate collection to shard set {shard0, shard1}.
        assertCreateCollection(db, coll.getName());
        assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));

        // Enable the balancer for the following operations, as it is needed for certain operations
        // that need move data between the shards, such as the 'removeShard' command.
        // The test will hang forever if the balancer is not enabled.
        withBalancerEnabled(() => {
            distributeCollectionDataOverShards(db, coll, {
                middle: {_id: 0},
                chunks: [
                    {find: {_id: -1}, to: st.shard0.shardName},
                    {find: {_id: 1}, to: st.shard1.shardName},
                ],
            });

            // Initially there should be 0 documents present.
            assertCollDataDistribution({
                [st.shard0.shardName]: 0,
                [st.shard1.shardName]: 0,
                [st.shard2.shardName]: 0,
            });

            jsTest.log.info("Inserting initial documents on {shard0, shard1}");

            // Insert documents into the collection so the documents are distributed to {shard0, shard1}.
            insertDocumentOnShard({_id: -1, a: -1}, st.shard0);
            insertDocumentOnShard({_id: 1, a: 1}, st.shard1);
            insertDocumentOnShard({_id: -3, a: -3}, st.shard0);

            assertCollDataDistribution({
                [st.shard0.shardName]: 2,
                [st.shard1.shardName]: 1,
                [st.shard2.shardName]: 0,
            });

            // Reshard collection and allocate to shards {shard1, shard2}. This also changes the
            // returned document keys for the events to include the new shard key field.
            jsTest.log.info("Resharding collection to {shard1, shard2}");
            assert.commandWorked(
                st.s.adminCommand({reshardCollection: coll.getFullName(), key: {a: 1}, numInitialChunks: 1}),
            );
            distributeCollectionDataOverShards(db, coll, {
                middle: {a: 0},
                chunks: [
                    {find: {a: -1}, to: st.shard1.shardName},
                    {find: {a: 1}, to: st.shard2.shardName},
                ],
            });
            assertCollDataDistribution({
                [st.shard0.shardName]: 0,
                [st.shard1.shardName]: 2,
                [st.shard2.shardName]: 1,
            });

            // Insert some documents into the collection, targeting the different shards.
            jsTest.log.info("Inserting more documents to {shard1, shard2}");
            insertDocumentOnShard({_id: 3, a: -1}, st.shard1);
            insertDocumentOnShard({_id: 2, a: 2}, st.shard2);
            insertDocumentOnShard({_id: 4, a: 1}, st.shard2);
            insertDocumentOnShard({_id: -2, a: -2}, st.shard1);

            assertCollDataDistribution({
                [st.shard0.shardName]: 0,
                [st.shard1.shardName]: 4,
                [st.shard2.shardName]: 3,
            });

            // Before decommissioning any shards, move the associated collections and databases away
            // from the shard to decommission, using the balancer.
            beforeDecommission();

            shardsToDecommission.forEach((shardToDecommission) => {
                // Remove and decommission a shard from the system.
                jsTest.log.info(`Decommissioning shard ${shardToDecommission}`);
                removeShard(st, shardToDecommission);
                jsTest.log.info(`Shard ${shardToDecommission} successfully decommissioned`);
            });
        });

        if (shardsToDecommission.length > 0) {
            jsTest.log.info(`Successfully decommissioned the requested shards`);
        }

        if (dropCollection) {
            // Drop collection.
            assertDropCollection(db, coll.getName());
        }
    }

    it("returns events in ignoreRemovedShards mode after shard has been removed", () => {
        // Enable sharding on the the test database and ensure that the primary is shard0.
        assert.commandWorked(
            db.adminCommand({
                enableSharding: db.getName(),
                primaryShard: st.shard0.shardName,
            }),
        );

        // Record high-watermark time marking the start point of the test.
        const startAtOperationTime = getClusterTime(db);

        // Execute the operations on the collection, make shard2 the primary shard for the database
        // and then remove shard0.
        executeCollectionOperations(
            [st.shard0.shardName],
            () => {
                assert.commandWorked(
                    st.s.adminCommand({
                        movePrimary: coll.getDB().getName(),
                        to: st.shard2.shardName,
                    }),
                );
            },
            true,
        );

        // Open a change stream and compare the events.
        csTest = new ChangeStreamTest(db);
        const csCursor = csTest.startWatchingChanges({
            pipeline: [
                {
                    $changeStream: {
                        version: "v2",
                        ignoreRemovedShards: true,
                        startAtOperationTime,
                    },
                },
            ],
            collection: coll,
        });

        // Read events until invalidation. We only expect to see events from shard1 and shard2.
        csTest.assertNextChangesEqual({
            cursor: csCursor,
            expectedChanges: [
                buildExpectedInsertEvent(st.shard1, {_id: 1, a: 1}, {_id: 1}),
                buildExpectedInsertEvent(st.shard1, {_id: 3, a: -1}),
                buildExpectedInsertEvent(st.shard2, {_id: 2, a: 2}),
                buildExpectedInsertEvent(st.shard2, {_id: 4, a: 1}),
                buildExpectedInsertEvent(st.shard1, {_id: -2, a: -2}),
                buildExpectedEventForNamespace("drop"),
                buildExpectedEventWithoutNamespace("invalidate"),
            ],
        });

        csTest.assertNoChange(csCursor);
    });

    it("returns events in ignoreRemovedShards mode after multiple shards have been removed", () => {
        // Enable sharding on the the test database and ensure that the primary is shard0.
        assert.commandWorked(
            db.adminCommand({
                enableSharding: db.getName(),
                primaryShard: st.shard0.shardName,
            }),
        );

        // Record high-watermark time marking the start point of the test.
        const startAtOperationTime = getClusterTime(db);

        // Execute the operations on the collection, make shard2 the primary shard for the database
        // and then remove shard0 and shard1.
        executeCollectionOperations(
            [st.shard0.shardName, st.shard1.shardName],
            () => {
                assert.commandWorked(
                    st.s.adminCommand({
                        movePrimary: coll.getDB().getName(),
                        to: st.shard2.shardName,
                    }),
                );
            },
            true,
        );

        // Open a change stream and compare the events.
        csTest = new ChangeStreamTest(db);
        const csCursor = csTest.startWatchingChanges({
            pipeline: [
                {
                    $changeStream: {
                        version: "v2",
                        ignoreRemovedShards: true,
                        startAtOperationTime,
                    },
                },
            ],
            collection: coll,
        });

        // Read events until invalidation. We only expect to see events from shard2.
        csTest.assertNextChangesEqual({
            cursor: csCursor,
            expectedChanges: [
                buildExpectedInsertEvent(st.shard2, {_id: 2, a: 2}),
                buildExpectedInsertEvent(st.shard2, {_id: 4, a: 1}),
                buildExpectedEventForNamespace("drop"),
                buildExpectedEventWithoutNamespace("invalidate"),
            ],
        });

        csTest.assertNoChange(csCursor);
    });

    it("returns events in ignoreRemovedShards mode after shard has been removed, showExpandedEvents", () => {
        // Enable sharding on the the test database and ensure that the primary is shard0.
        assert.commandWorked(
            db.adminCommand({
                enableSharding: db.getName(),
                primaryShard: st.shard0.shardName,
            }),
        );

        // Record high-watermark time marking the start point of the test.
        const startAtOperationTime = getClusterTime(db);

        // Execute the operations on the collection and then remove shard2.
        executeCollectionOperations([st.shard2.shardName], () => {}, true);

        // Open a change stream and compare the events.
        csTest = new ChangeStreamTest(db);
        const csCursor = csTest.startWatchingChanges({
            pipeline: [
                {
                    $changeStream: {
                        version: "v2",
                        ignoreRemovedShards: true,
                        showExpandedEvents: true,
                        startAtOperationTime,
                    },
                },
            ],
            collection: coll,
        });

        // Read events until invalidation. We only expect to see events from shard0 and shard1.
        csTest.assertNextChangesEqual({
            cursor: csCursor,
            expectedChanges: [
                buildExpectedEventForNamespace("create"),
                buildExpectedEventForNamespace("shardCollection"),
                buildExpectedInsertEvent(st.shard0, {_id: -1, a: -1}, {_id: -1}),
                buildExpectedInsertEvent(st.shard1, {_id: 1, a: 1}, {_id: 1}),
                buildExpectedInsertEvent(st.shard0, {_id: -3, a: -3}, {_id: -3}),
                buildExpectedEventForNamespace("reshardCollection"),
                buildExpectedInsertEvent(st.shard1, {_id: 3, a: -1}),
                buildExpectedInsertEvent(st.shard1, {_id: -2, a: -2}),
                buildExpectedEventForNamespace("drop"),
                buildExpectedEventWithoutNamespace("invalidate"),
            ],
        });

        csTest.assertNoChange(csCursor);
    });

    it("returns only new events in ignoreRemovedShards mode after all original shards were decommissioned", () => {
        // Enable sharding on the the test database and ensure that the primary is shard0.
        assert.commandWorked(
            db.adminCommand({
                enableSharding: db.getName(),
                primaryShard: st.shard0.shardName,
            }),
        );

        // Record high-watermark time marking the start point of the test.
        const startAtOperationTime = getClusterTime(db);

        let newShard = null;

        // Execute the operations on the collection, add a new shard and make this new shard the
        // primary shard for the database. Then decommission shard0, shard1 and shard2.
        executeCollectionOperations(
            [st.shard0.shardName, st.shard1.shardName, st.shard2.shardName],
            () => {
                newShard = new ReplSetTest({
                    nodes: 1,
                    setParameter: {
                        writePeriodicNoops: true,
                        periodicNoopIntervalSecs: 1,
                    },
                });

                newShard.startSet({shardsvr: ""});
                newShard.initiate();

                let res = assert.commandWorked(
                    st.s.adminCommand({
                        addShard: newShard.getURL(),
                        name: newShard.name,
                    }),
                );
                newShard.shardName = res.shardAdded;

                jsTest.log.info(`Added new shard: ${newShard.shardName}`);

                // Make the new shard the primary for the database.
                assert.commandWorked(
                    st.s.adminCommand({
                        movePrimary: coll.getDB().getName(),
                        to: newShard.shardName,
                    }),
                );

                // Make the new shard responsible for all chunks, including any newly inserted
                // documents.
                distributeCollectionDataOverShards(db, coll, {
                    middle: {a: 0},
                    chunks: [
                        {find: {a: -100}, to: st.shard3.shardName},
                        {find: {a: 0}, to: newShard.shardName},
                    ],
                });
            },
            false,
        );

        assert.neq(newShard, null, "Expected a new shard to have been added");
        shardsAdded.push(newShard);

        // Open a change stream and compare the events.
        csTest = new ChangeStreamTest(db);
        const csCursor = csTest.startWatchingChanges({
            pipeline: [
                {
                    $changeStream: {
                        version: "v2",
                        ignoreRemovedShards: true,
                        startAtOperationTime,
                    },
                },
            ],
            collection: coll,
        });

        // There should be no events returned since all original shards have been decommissioned.
        csTest.assertNoChange(csCursor);

        // Insert a few documents on the just-added shard.
        insertDocumentOnShard({_id: 6, a: 6}, newShard);
        insertDocumentOnShard({_id: 7, a: 7}, newShard);
        coll.drop();

        // Read the newly inserted document from the just-added shard.
        csTest.assertNextChangesEqual({
            cursor: csCursor,
            expectedChanges: [
                buildExpectedInsertEvent(newShard, {_id: 6, a: 6}),
                buildExpectedInsertEvent(newShard, {_id: 7, a: 7}),
                buildExpectedEventForNamespace("drop"),
                buildExpectedEventWithoutNamespace("invalidate"),
            ],
        });

        csTest.assertNoChange(csCursor);
    });

    it("does not return events in ignoreRemovedShards mode for a non-existing database", () => {
        // Record high-watermark time marking the start point of the test.
        const startAtOperationTime = getClusterTime(db);

        // Open a change stream on a non-existing collection in ignoreRemovedShards mode from the
        // original start point, and assume that there are no events.
        csTest = new ChangeStreamTest(db);
        const csCursor = csTest.startWatchingChanges({
            pipeline: [
                {
                    $changeStream: {
                        version: "v2",
                        ignoreRemovedShards: true,
                        startAtOperationTime,
                    },
                },
            ],
            collection: coll.getName(),
        });

        csTest.assertNoChange(csCursor);
    });

    it("does not return events in ignoreRemovedShards mode for a non-existing collection", () => {
        // Enable sharding on the the test database and ensure that the primary is shard0.
        assert.commandWorked(
            db.adminCommand({
                enableSharding: db.getName(),
                primaryShard: st.shard0.shardName,
            }),
        );

        // Record high-watermark time marking the start point of the test.
        const startAtOperationTime = getClusterTime(db);

        // Open a change stream on a non-existing collection in ignoreRemovedShards mode from the
        // original start point, and assume that there are no events.
        csTest = new ChangeStreamTest(db);
        const csCursor = csTest.startWatchingChanges({
            pipeline: [
                {
                    $changeStream: {
                        version: "v2",
                        ignoreRemovedShards: true,
                        startAtOperationTime,
                    },
                },
            ],
            collection: coll.getName() + "-does-not-exist",
        });

        csTest.assertNoChange(csCursor);
    });
});
