/**
 * Stresses the timing of $_internalExchange by driving each consumer cursor from its own parallel
 * shell that sleeps a random number of milliseconds between getMores.
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

// Each iteration is a full aggregate + drain cycle. Several cheap iterations explore more
// interleavings than one expensive one, since the orderings that matter are established in the
// first few getMores while the consumers are still contending for the producer.
const kNumIterations = 5;
const kDocsPerConsumer = 100;
const kGetMoreBatchSize = 10;

// The upper bound on the per-getMore sleep. Sleeps are drawn from [0, kMaxSleepMs), so a consumer
// that draws 0 races ahead while one that draws near the maximum falls far behind and forces the
// others to fill and drain the exchange buffer without it.
const kMaxSleepMs = 25;

describe("exchange under randomized getMore timing", function () {
    let conn;
    let db;
    let coll;
    let resultsColl;

    before(function () {
        // Seeds from TestData.seed when resmoke supplies one and logs whatever it picked, so a
        // failing schedule can be replayed.
        Random.setRandomSeed();

        conn = MongoRunner.runMongod({
            setParameter: {
                // Needed to avoid spilling to disk, which changes memory metrics.
                allowDiskUseByDefault: false,
                // Needed so that chunked memory tracking reaches CurOp.
                internalQueryMaxWriteToCurOpMemoryUsageBytes: 256,
            },
        });
        assert.neq(null, conn, "mongod was unable to start up");

        db = conn.getDB("test");
        db.dropDatabase();
        coll = db[jsTestName()];
        resultsColl = db[jsTestName() + "_results"];

        // Log every operation, so that the memory metrics assertions below have profiler entries to
        // read regardless of how slow any individual getMore happened to be.
        db.setProfilingLevel(2, {slowms: -1});
    });

    after(function () {
        if (conn) {
            db.setProfilingLevel(0);
            db.dropDatabase();
            MongoRunner.stopMongod(conn);
        }
    });

    /**
     * Opens an exchange with 'nConsumers' consumers over 'coll' and returns the cursor ids, along
     * with the session they must be iterated under. Exchange is an internal-client-only feature, so
     * this runs over a separate connection that has identified itself as an internal client.
     */
    function openExchange(nConsumers) {
        const internalConn = new Mongo(db.getMongo().host);
        assert.commandWorked(
            internalConn.getDB("admin").runCommand({
                hello: 1,
                internalClient: {minWireVersion: NumberInt(0), maxWireVersion: NumberInt(7)},
            }),
        );

        const session = internalConn.startSession();
        const aggResult = assert.commandWorked(
            session.getDatabase(db.getName()).runCommand({
                aggregate: coll.getName(),
                pipeline: [
                    // Also include a $unionWith to exercise memory tracking in a subpipeline.
                    {
                        $unionWith: {
                            coll: coll.getName(),
                            pipeline: [
                                {
                                    $group: {
                                        _id: {$concat: [{$toString: "$_id"}, "Y"]},
                                        docId: {$first: "$_id"},
                                        b: {$first: "$b"},
                                    },
                                },
                                {$replaceWith: {_id: "$docId", b: "$b"}},
                                {$sort: {_id: 1}},
                            ],
                        },
                    },
                    {
                        $group: {
                            _id: {$concat: [{$toString: "$_id"}, "X"]},
                            v0: {$first: "$_id"},
                            v1: {$first: "$b"},
                        },
                    },
                ],
                cursor: {batchSize: 0},
                exchange: {
                    policy: "roundrobin",
                    consumers: NumberInt(nConsumers),
                    orderPreserving: false,
                    bufferSize: NumberInt(128),
                    key: {},
                },
                readConcern: {},
                writeConcern: {},
            }),
        );

        assert(aggResult.cursors, "expected cursors array in aggregate result", {aggResult});
        assert.eq(aggResult.cursors.length, nConsumers, "unexpected number of cursors", {
            aggResult,
        });

        return {
            session,
            cursorIds: aggResult.cursors.map((c) => c.cursor.id),
        };
    }

    /**
     * Runs in a parallel shell. Drains one consumer cursor, sleeping a random interval before each
     * getMore, and records the '_id's it saw into 'resultsCollName' so the parent shell can check
     * that the consumers between them returned every document exactly once.
     */
    function drainConsumerWithRandomSleeps(args) {
        // Seed per shell, so a failure can be replayed by pinning this seed.
        Random.setRandomSeed(args.seed);

        const seenIds = [];
        while (true) {
            // Sleep *before* the getMore rather than after, so that the very first getMore -- the
            // one that decides which consumer runs the producer subpipeline first -- is also
            // subject to the randomized delay.
            sleep(Random.randInt(args.maxSleepMs));

            const getMoreResult = assert.commandWorked(
                db.runCommand({
                    getMore: args.cursorId,
                    collection: args.collName,
                    batchSize: NumberInt(args.batchSize),
                    lsid: args.sessionId,
                }),
                `consumer ${args.consumerIndex} getMore failed`,
            );

            const batch = getMoreResult.cursor.nextBatch;
            for (const doc of batch) {
                seenIds.push(doc.v0);
            }

            // A zero-length batch means this consumer is exhausted.
            if (batch.length === 0) {
                break;
            }
        }

        assert.commandWorked(
            db.getCollection(args.resultsCollName).insertOne({
                iteration: args.iteration,
                consumerIndex: args.consumerIndex,
                seenIds: seenIds,
            }),
        );
    }

    /**
     * Drains all of 'cursorIds' concurrently, one parallel shell per cursor, and joins them.
     */
    function drainAllConsumers({cursorIds, session, iteration, seed}) {
        const shells = cursorIds.map((cursorId, consumerIndex) =>
            startParallelShell(
                funWithArgs(drainConsumerWithRandomSleeps, {
                    collName: coll.getName(),
                    resultsCollName: resultsColl.getName(),
                    cursorId: cursorId,
                    consumerIndex: consumerIndex,
                    iteration: iteration,
                    sessionId: session.getSessionId(),
                    batchSize: kGetMoreBatchSize,
                    maxSleepMs: kMaxSleepMs,
                    // Distinct per shell so the consumers do not draw identical sleep sequences,
                    // which would reintroduce the lockstep cadence we are trying to avoid.
                    seed: seed + consumerIndex,
                }),
                conn.port,
            ),
        );

        // Join every shell before asserting, so that one consumer failing does not leave the others
        // running against a mongod the test is about to tear down.
        for (const shell of shells) {
            shell();
        }
    }

    it("returns every document exactly once and still reports memory metrics", function () {
        for (let iteration = 0; iteration < kNumIterations; ++iteration) {
            // Vary the consumer count across iterations: two consumers maximizes the chance that
            // one is stalled while the other drives the producer, while larger counts exercise
            // contention between several waiters.
            const nConsumers = 2 + (iteration % 3);
            const docCount = nConsumers * kDocsPerConsumer;

            coll.drop();
            resultsColl.drop();
            const bulk = coll.initializeUnorderedBulkOp();
            for (let i = 0; i < docCount; ++i) {
                bulk.insert({_id: i, b: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"});
            }
            assert.commandWorked(bulk.execute());

            // Derived from the global seed, so `resmoke --seed=...` replays the whole schedule.
            const seed = Random.randInt(1 << 30);
            jsTest.log.info("Starting exchange timing iteration", {iteration, nConsumers, seed});

            const {session, cursorIds} = openExchange(nConsumers);
            try {
                drainAllConsumers({cursorIds, session, iteration, seed});

                const perConsumer = resultsColl.find({iteration: iteration}).toArray();
                assert.eq(perConsumer.length, nConsumers, "a consumer failed to record its results", {
                    perConsumer,
                });

                const allIds = perConsumer.flatMap((entry) => entry.seenIds).sort((a, b) => a - b);
                assert.eq(allIds.length, docCount, "consumers returned the wrong number of documents", {
                    counts: perConsumer.map((e) => ({
                        consumerIndex: e.consumerIndex,
                        count: e.seenIds.length,
                    })),
                });
                for (let i = 0; i < docCount; ++i) {
                    assert.eq(allIds[i], i, "document lost or duplicated across consumers", {
                        index: i,
                        actual: allIds[i],
                    });
                }
                //Check consumer0 for memory tracking information.
                const entries = db.system.profile
                    .find({cursorid: cursorIds[0], peakTrackedMemBytes: {$gt: 0}})
                    .toArray();
                assert.gt(
                    entries.length,
                    0,
                    "expected at least one profiler entry reporting a non-zero peakTrackedMemBytes for consumer 0",
                    {cursorId: cursorIds[0]},
                );
                // TODO SERVER-132671: Assert that other consumers don't report memory.
            } finally {
                session.endSession();
            }
        }
    });
});
