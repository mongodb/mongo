/**
 * Tests execution control deprioritization mechanisms including:
 *      - Heuristic deprioritization for long-running user operations.
 *      - Background task deprioritization for index builds.
 *      - Background task deprioritization for TTL deletions.
 *      - Background task deprioritization for range deletions in sharded clusters.
 *
 * @tags: [
 *   # The test deploys replica sets with execution control concurrency adjustment configured by
 *   # each test case, which should not be overwritten and expect to have 'throughputProbing' as
 *   # default.
 *   incompatible_with_execution_control_with_prioritization,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {getWinningPlanFromExplain, isCollscan, isIxscan} from "jstests/libs/query/analyze_plan.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    generateRandomString,
    getLowPriorityReadCount,
    getLowPriorityWriteCount,
    insertTestDocuments,
    kFixedConcurrentTransactionsWithPrioritizationAlgorithm,
    setBackgroundTaskDeprioritization,
} from "jstests/noPassthrough/admission/execution_control/libs/execution_control_helper.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

describe("Execution control deprioritization mechanisms", function () {
    const kNumDocs = 1000;

    describe("Heuristic deprioritization for long-running operations", function () {
        let replTest, primary, db, coll;

        before(function () {
            replTest = new ReplSetTest({
                nodes: 1,
                nodeOptions: {
                    setParameter: {
                        // Force the query to yield frequently to better expose the low-priority
                        // behavior.
                        internalQueryExecYieldIterations: 1,
                        executionControlConcurrencyAdjustmentAlgorithm:
                            kFixedConcurrentTransactionsWithPrioritizationAlgorithm,
                    },
                },
            });
            replTest.startSet();
            replTest.initiate();
            primary = replTest.getPrimary();
            db = primary.getDB(jsTestName());
            coll = db.coll;

            insertTestDocuments(coll, kNumDocs, {payloadSize: 256, includeRandomString: true, randomStringLength: 100});

            assert.commandWorked(coll.createIndex({payload: 1}));
            assert.commandWorked(coll.createIndex({randomStr: 1}));
        });

        after(function () {
            replTest.stopSet();
        });

        it("should deprioritize unbounded forward collection scans", function () {
            const before = getLowPriorityReadCount(primary);
            const explain = coll.find().hint({$natural: 1}).explain();
            assert(isCollscan(db, getWinningPlanFromExplain(explain)));

            assert.eq(kNumDocs, coll.find().hint({$natural: 1}).itcount());
            assert.gt(getLowPriorityReadCount(primary), before);
        });

        it("should deprioritize unbounded backward collection scans", function () {
            const before = getLowPriorityReadCount(primary);
            const explain = coll.find().hint({$natural: -1}).explain();
            assert(isCollscan(db, getWinningPlanFromExplain(explain)));

            assert.eq(kNumDocs, coll.find().hint({$natural: -1}).itcount());
            assert.gt(getLowPriorityReadCount(primary), before);
        });

        it("should deprioritize unbounded index scans", function () {
            const before = getLowPriorityReadCount(primary);
            const query = {payload: {$gte: ""}};
            const explain = coll.find(query).sort({payload: 1}).explain();
            assert(isIxscan(db, getWinningPlanFromExplain(explain)));

            assert.eq(kNumDocs, coll.find(query).sort({payload: 1}).itcount());
            assert.gt(getLowPriorityReadCount(primary), before);
        });

        it("should deprioritize long-running regex on collection scans", function () {
            const before = getLowPriorityReadCount(primary);
            const query = {randomStr: {$regex: "a.*b.*c.*d"}};
            const explain = coll.find(query).hint({$natural: 1}).explain();
            assert(isCollscan(db, getWinningPlanFromExplain(explain)));

            coll.find(query).hint({$natural: 1}).itcount();
            assert.gt(getLowPriorityReadCount(primary), before);
        });

        it("should deprioritize long-running regex on index scans", function () {
            const before = getLowPriorityReadCount(primary);
            const query = {randomStr: {$regex: "a.*b.*c.*d"}};
            const explain = coll.find(query).explain();
            assert(isIxscan(db, getWinningPlanFromExplain(explain)));

            coll.find(query).itcount();
            assert.gt(getLowPriorityReadCount(primary), before);
        });

        it("should NOT deprioritize multi-document transactions", function () {
            // Even though this transaction contains an operation that would normally be
            // deprioritized (an unbounded collection scan), the transaction itself should run with
            // normal priority.
            const before = getLowPriorityReadCount(primary);

            const session = db.getMongo().startSession();
            const sessionColl = session.getDatabase(db.getName()).getCollection(coll.getName());

            session.startTransaction();
            // Perform an unbounded collection scan within the transaction.
            assert.eq(kNumDocs, sessionColl.find().hint({$natural: 1}).itcount());
            assert.commandWorked(session.commitTransaction_forTesting());
            session.endSession();

            assert.eq(getLowPriorityReadCount(primary), before);
        });

        it("should deprioritize multi-inserts", function () {
            const before = getLowPriorityWriteCount(primary);

            // Create a large batch of documents to insert.
            const batchSize = 1000;
            const docs = [];
            for (let i = kNumDocs; i < kNumDocs + batchSize; i++) {
                docs.push({_id: i, payload: "x".repeat(512), randomStr: generateRandomString(100)});
            }

            // Perform multi-inserts which should be deprioritized due to their size.
            assert.commandWorked(coll.insertMany(docs));

            assert.gt(getLowPriorityWriteCount(primary), before);
        });

        it("should deprioritize multi-updates", function () {
            const before = getLowPriorityWriteCount(primary);

            const result = coll.updateMany({payload: {$exists: true}}, {$set: {updated: true}});
            assert.gt(result.modifiedCount, 100);

            assert.gt(getLowPriorityWriteCount(primary), before);
        });

        it("should deprioritize bulk writes", function () {
            const before = getLowPriorityWriteCount(primary);

            // Create a bulk write operation with mixed operations.
            const bulk = coll.initializeUnorderedBulkOp();

            // Add some inserts.
            const startId = kNumDocs + 1000;
            for (let i = startId; i < startId + 200; i++) {
                bulk.insert({_id: i, bulkInsert: true, payload: "y".repeat(256)});
            }

            // Add some updates.
            bulk.find({payload: {$regex: /^x/}}).update({$set: {bulkUpdated: true}});

            // Add some deletes.
            bulk.find({_id: {$gte: kNumDocs - 50, $lt: kNumDocs}}).remove();

            // Execute the bulk write.
            assert.commandWorked(bulk.execute());

            assert.gt(getLowPriorityWriteCount(primary), before);
        });

        it("should deprioritize multi-deletes", function () {
            const before = getLowPriorityWriteCount(primary);

            // Insert some documents to delete.
            const docsToDelete = [];
            for (let i = 0; i < 100; i++) {
                docsToDelete.push({_id: `delete_${i}`, toDelete: true, payload: "z".repeat(128)});
            }
            assert.commandWorked(coll.insertMany(docsToDelete));

            const deleteResult = coll.deleteMany({toDelete: true});
            assert.gt(deleteResult.deletedCount, 50);

            assert.gt(getLowPriorityWriteCount(primary), before);
        });
    });

    describe("Background task deprioritization for index builds", function () {
        let replTest, primary, secondary, db, coll, secondaryDB;

        before(function () {
            replTest = new ReplSetTest({
                nodes: 2,
                nodeOptions: {
                    setParameter: {
                        executionControlConcurrencyAdjustmentAlgorithm:
                            kFixedConcurrentTransactionsWithPrioritizationAlgorithm,
                        executionControlHeuristicDeprioritizationEnabled: false,
                    },
                },
            });
            replTest.startSet();
            replTest.initiate();
            primary = replTest.getPrimary();
            secondary = replTest.getSecondary();
            db = primary.getDB(jsTestName());
            coll = db.coll;
            secondaryDB = secondary.getDB(jsTestName());

            insertTestDocuments(coll, kNumDocs, {
                payloadSize: 256,
                docGenerator: (i, payload) => ({_id: i, x: `value_${i}`, payload: payload}),
            });
        });

        after(function () {
            replTest.stopSet();
        });

        it("should use low priority for index builds when deprioritization is enabled", function () {
            const primaryBefore = getLowPriorityWriteCount(primary);
            const secondaryBefore = getLowPriorityWriteCount(secondary);

            // Build the index. This command blocks until the index is ready on the primary.
            assert.commandWorked(coll.createIndex({x: 1}));

            // Wait for the index build to complete on the secondary.
            IndexBuildTest.waitForIndexBuildToStop(secondaryDB);

            assert.gt(getLowPriorityWriteCount(primary), primaryBefore);
            assert.gt(getLowPriorityWriteCount(secondary), secondaryBefore);
        });

        it("should use normal priority for index builds when deprioritization is disabled", function () {
            setBackgroundTaskDeprioritization(primary, false);
            setBackgroundTaskDeprioritization(secondary, false);

            const primaryBefore = getLowPriorityWriteCount(primary);
            const secondaryBefore = getLowPriorityWriteCount(secondary);

            assert.commandWorked(coll.createIndex({y: 1}));
            IndexBuildTest.waitForIndexBuildToStop(secondaryDB);

            assert.eq(getLowPriorityWriteCount(primary), primaryBefore);
            assert.eq(getLowPriorityWriteCount(secondary), secondaryBefore);
        });
    });

    describe("Background task deprioritization for TTL deletions", function () {
        let replTest, primary, db, coll;

        before(function () {
            replTest = new ReplSetTest({
                nodes: 1,
                nodeOptions: {
                    setParameter: {
                        ttlMonitorSleepSecs: 1,
                        ttlMonitorEnabled: false,
                        executionControlConcurrencyAdjustmentAlgorithm:
                            kFixedConcurrentTransactionsWithPrioritizationAlgorithm,
                        executionControlHeuristicDeprioritizationEnabled: false,
                    },
                },
            });
            replTest.startSet();
            replTest.initiate();
            primary = replTest.getPrimary();
            db = primary.getDB(jsTestName());
            coll = db.coll;

            // Create a TTL index to expire documents after 0 seconds.
            assert.commandWorked(coll.createIndex({expireAt: 1}, {expireAfterSeconds: 0}));
        });

        after(function () {
            replTest.stopSet();
        });

        /**
         * Inserts a specified number of expired documents into the collection.
         */
        function insertExpiredDocs(numDocs, startId = 0) {
            const pastDate = new Date(Date.now() - 5000);
            insertTestDocuments(coll, numDocs, {
                startId,
                docGenerator: (id, payload) => ({_id: id, expireAt: pastDate, payload}),
            });
        }

        /**
         * Waits for the TTL monitor to clear all documents from the collection.
         */
        function waitForTTLDeletion() {
            assert.soon(
                () => coll.countDocuments({}) === 0,
                "TTL monitor did not delete documents",
                10 * 1000 /* 10 seconds */,
            );
        }

        function enableTTLMonitor(enabled) {
            assert.commandWorked(primary.adminCommand({setParameter: 1, ttlMonitorEnabled: enabled}));
        }

        it("should use low priority for TTL deletions when deprioritization is enabled", function () {
            insertExpiredDocs(kNumDocs, 0);
            const before = getLowPriorityWriteCount(primary);

            enableTTLMonitor(true);
            waitForTTLDeletion();
            enableTTLMonitor(false);

            assert.gt(getLowPriorityWriteCount(primary), before);
        });

        it("should use normal priority for TTL deletions when deprioritization is disabled", function () {
            setBackgroundTaskDeprioritization(primary, false);

            insertExpiredDocs(kNumDocs, kNumDocs);
            const before = getLowPriorityWriteCount(primary);

            enableTTLMonitor(true);
            waitForTTLDeletion();

            assert.eq(getLowPriorityWriteCount(primary), before);
        });
    });

    describe("Background task deprioritization for range deletions", function () {
        let st, donor, recipient, dbName, coll, ns;

        before(function () {
            st = new ShardingTest({
                shards: 2,
                other: {
                    rsOptions: {
                        setParameter: {
                            executionControlConcurrencyAdjustmentAlgorithm:
                                kFixedConcurrentTransactionsWithPrioritizationAlgorithm,
                            executionControlHeuristicDeprioritizationEnabled: false,
                        },
                    },
                },
            });

            dbName = jsTestName();
            coll = st.s.getDB(dbName).coll;
            ns = coll.getFullName();

            assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
            assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

            insertTestDocuments(coll, kNumDocs, {payloadSize: 256});

            assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: kNumDocs / 2}}));

            donor = st.shard0;
            recipient = st.shard1;
        });

        after(function () {
            st.stop();
        });

        it("should use low priority for range deletions when deprioritization is enabled", function () {
            const donorBefore = getLowPriorityWriteCount(donor);
            assert.commandWorked(
                st.s.adminCommand({
                    moveChunk: ns,
                    find: {_id: 0},
                    to: recipient.shardName,
                    _waitForDelete: true,
                }),
            );

            assert.gt(getLowPriorityWriteCount(donor), donorBefore);
        });

        it("should use normal priority for range deletions when deprioritization is disabled", function () {
            setBackgroundTaskDeprioritization(recipient, false);

            const recipientBefore = getLowPriorityWriteCount(recipient);
            assert.commandWorked(
                st.s.adminCommand({
                    moveChunk: ns,
                    find: {_id: 0},
                    to: donor.shardName,
                    _waitForDelete: true,
                }),
            );

            assert.eq(getLowPriorityWriteCount(recipient), recipientBefore);
        });
    });
});
