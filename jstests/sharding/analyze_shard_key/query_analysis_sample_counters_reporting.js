/**
 * Test output of "query analyzer" section of currentOp and serverStatus commands during query
 * sampling.
 *
 * @tags: [requires_fcv_63, featureFlagAnalyzeShardKey]
 */

(function() {
"use strict";

// load("jstests/libs/analyze_shard_key_util.js");
load("jstests/sharding/analyze_shard_key/libs/query_sampling_util.js");

const dbName = "testDb";
const collName = "testColl";
const nss = dbName + '.' + collName;
const kNumDocs = 10;
const kSampleRate = kNumDocs * 10;

const opKindRead = 0;
const opKindWrite = 1;

function getCurrentOpAndServerStatus(st) {
    const mongosCurrOp = st.s0.getDB("admin")
                             .aggregate([
                                 {$currentOp: {allUsers: true, localOps: true}},
                                 {$match: {desc: "query analyzer"}}
                             ])
                             .toArray();
    const mongodCurrOp = assert
                             .commandWorked(st.rs0.getPrimary().adminCommand(
                                 {currentOp: true, desc: "query analyzer"}))
                             .inprog;
    const mongosServerStatus = assert.commandWorked(st.s0.adminCommand({serverStatus: 1}));
    const mongodServerStatus =
        assert.commandWorked(st.rs0.getPrimary().adminCommand({serverStatus: 1}));
    return {
        "currentOp": {"mongos": mongosCurrOp, "mongod": mongodCurrOp},
        "serverStatus": {
            "mongos": mongosServerStatus.queryAnalyzers,
            "mongod": mongodServerStatus.queryAnalyzers
        }
    };
}

/**
 * Runs a db command, and compares resulting currentOp and serverStatus to oldState, the initial
 * state before running the command.
 * Returns the output of currentOp and serverStatus of both mongod and mongos.
 */
function runCommandAndAssertCurrentOpAndServerStatus(st, db, command, opKind, oldState) {
    assert.commandWorked(db.runCommand(command));

    const newState = getCurrentOpAndServerStatus(st);

    assert.eq(newState.currentOp.mongos.length, 1);
    assert.eq(newState.currentOp.mongod.length, 1);

    // Verify mongos currentOp and serverStatus.

    assert.eq(oldState.currentOp.mongos.length,
              newState.currentOp.mongos.length,
              tojson([oldState.currentOp.mongos, newState.currentOp.mongos]));
    assert.eq(oldState.serverStatus.mongos.activeCollections,
              newState.serverStatus.mongos.activeCollections,
              tojson([oldState.serverStatus.mongos, newState.serverStatus.mongos]));
    assert.eq(oldState.serverStatus.mongos.totalCollections,
              newState.serverStatus.mongos.totalCollections,
              tojson([oldState.serverStatus.mongos, newState.serverStatus.mongos]));

    if (opKind === opKindRead) {
        assert.eq(oldState.currentOp.mongos[0].sampledReadsCount + 1,
                  newState.currentOp.mongos[0].sampledReadsCount,
                  tojson([oldState.currentOp.mongos, newState.currentOp.mongos]));
        assert.eq(oldState.currentOp.mongos[0].sampledWritesCount,
                  newState.currentOp.mongos[0].sampledWritesCount,
                  tojson([oldState.currentOp.mongos, newState.currentOp.mongos]));

        assert.eq(oldState.serverStatus.mongos.totalSampledReadsCount + 1,
                  newState.serverStatus.mongos.totalSampledReadsCount,
                  tojson([oldState.serverStatus.mongos, newState.serverStatus.mongos]));
        assert.eq(oldState.serverStatus.mongos.totalSampledWritesCount,
                  newState.serverStatus.mongos.totalSampledWritesCount,
                  tojson([oldState.serverStatus.mongos, newState.serverStatus.mongos]));
    } else if (opKind === opKindWrite) {
        assert.eq(oldState.currentOp.mongos[0].sampledReadsCount,
                  newState.currentOp.mongos[0].sampledReadsCount,
                  tojson([oldState.currentOp.mongos, newState.currentOp.mongos]));
        assert.eq(oldState.currentOp.mongos[0].sampledWritesCount + 1,
                  newState.currentOp.mongos[0].sampledWritesCount,
                  tojson([oldState.currentOp.mongos, newState.currentOp.mongos]));

        assert.eq(oldState.serverStatus.mongos.totalSampledReadsCount,
                  newState.serverStatus.mongos.totalSampledReadsCount,
                  tojson([oldState.serverStatus.mongos, newState.serverStatus.mongos]));
        assert.eq(oldState.serverStatus.mongos.totalSampledWritesCount + 1,
                  newState.serverStatus.mongos.totalSampledWritesCount,
                  tojson([oldState.serverStatus.mongos, newState.serverStatus.mongos]));
    } else {
        throw new Error("Unknown operation kind " + opKind);
    }

    // Verify mongod currentOp and serverStatus.

    assert.eq(oldState.currentOp.mongod.length,
              newState.currentOp.mongod.length,
              tojson([oldState.currentOp.mongod, newState.currentOp.mongod]));
    assert.eq(oldState.serverStatus.mongod.totalCollections,
              newState.serverStatus.mongod.totalCollections,
              tojson([oldState.serverStatus.mongod, newState.serverStatus.mongod]));

    if (opKind == opKindRead) {
        // QueryAnalysisWriter (on mongod) updates its counters on a separate thread, so we need
        // to use assert.soon. After this counter is updated, we don't need to wait to check the
        // other counters.
        assert.soon(() => {
            return oldState.currentOp.mongod[0].sampledReadsCount + 1 ==
                newState.currentOp.mongod[0].sampledReadsCount;
        }, tojson([oldState.currentOp.mongod[0], newState.currentOp.mongod[0]]));
        assert.eq(oldState.currentOp.mongod[0].sampledWritesCount,
                  newState.currentOp.mongod[0].sampledWritesCount,
                  tojson([oldState.currentOp.mongod[0], newState.currentOp.mongod[0]]));

        // Instead of figuring out the size of the sample being written, just make sure
        // that the byte counter is greater than before.
        assert.lt(oldState.currentOp.mongod[0].sampledReadsBytes,
                  newState.currentOp.mongod[0].sampledReadsBytes,
                  tojson([oldState.currentOp.mongod[0], newState.currentOp.mongod[0]]));
        assert.eq(oldState.currentOp.mongod[0].sampledWritesBytes,
                  newState.currentOp.mongod[0].sampledWritesBytes,
                  tojson([oldState.currentOp.mongod[0], newState.currentOp.mongod[0]]));

        assert.eq(oldState.serverStatus.mongod.totalSampledReadsCount + 1,
                  newState.serverStatus.mongod.totalSampledReadsCount,
                  tojson([oldState.serverStatus.mongod[0], newState.serverStatus.mongod[0]]));
        assert.eq(oldState.serverStatus.mongod.totalSampledWritesCount,
                  newState.serverStatus.mongod.totalSampledWritesCount,
                  tojson([oldState.serverStatus.mongod[0], newState.serverStatus.mongod[0]]));
        assert.lt(oldState.serverStatus.mongod.totalSampledReadsBytes,
                  newState.serverStatus.mongod.totalSampledReadsBytes,
                  tojson([oldState.serverStatus.mongod[0], newState.serverStatus.mongod[0]]));
        assert.eq(oldState.serverStatus.mongod.totalSampledWritesBytes,
                  newState.serverStatus.mongod.totalSampledWritesBytes,
                  tojson([oldState.serverStatus.mongod[0], newState.serverStatus.mongod[0]]));
    } else if (opKind == opKindWrite) {
        // QueryAnalysisWriter (on mongod) updates its counters on a separate thread, so we need
        // to use assert.soon. After this counter is updated, we don't need to wait to check the
        // other counters.
        assert.soon(() => {
            return oldState.currentOp.mongod[0].sampledWritesCount + 1 ==
                newState.currentOp.mongod[0].sampledWritesCount;
        }, tojson([oldState.currentOp.mongod[0], newState.currentOp.mongod[0]]));
        assert.eq(oldState.currentOp.mongod[0].sampledReadsCount,
                  newState.currentOp.mongod[0].sampledReadsCount,
                  tojson([oldState.currentOp.mongod[0], newState.currentOp.mongod[0]]));
        assert.eq(oldState.currentOp.mongod[0].sampledReadsBytes,
                  newState.currentOp.mongod[0].sampledReadsBytes,
                  tojson([oldState.currentOp.mongod[0], newState.currentOp.mongod[0]]));
        assert.lt(oldState.currentOp.mongod[0].sampledWritesBytes,
                  newState.currentOp.mongod[0].sampledWritesBytes,
                  tojson([oldState.currentOp.mongod[0], newState.currentOp.mongod[0]]));

        assert.eq(oldState.serverStatus.mongod.totalSampledReadsCount,
                  newState.serverStatus.mongod.totalSampledReadsCount,
                  tojson([oldState.serverStatus.mongod[0], newState.serverStatus.mongod[0]]));
        assert.eq(oldState.serverStatus.mongod.totalSampledWritesCount + 1,
                  newState.serverStatus.mongod.totalSampledWritesCount,
                  tojson([oldState.serverStatus.mongod[0], newState.serverStatus.mongod[0]]));
        assert.eq(oldState.serverStatus.mongod.totalSampledReadsBytes,
                  newState.serverStatus.mongod.totalSampledReadsBytes,
                  tojson([oldState.serverStatus.mongod[0], newState.serverStatus.mongod[0]]));
        assert.lt(oldState.serverStatus.mongod.totalSampledWritesBytes,
                  newState.serverStatus.mongod.totalSampledWritesBytes,
                  tojson([oldState.serverStatus.mongod[0], newState.serverStatus.mongod[0]]));
    } else {
        throw new Error("Unknown operation kind " + opKind);
    }

    return newState;
}

function testCurrentOpAndServerStatusBasic() {
    const st = new ShardingTest({
        shards: 1,
        mongos: 1,
        rs: {
            nodes: 1,
            setParameter: {
                queryAnalysisWriterIntervalSecs: 1,
            }
        },
        mongosOptions: {
            setParameter: {
                queryAnalysisSamplerConfigurationRefreshSecs: 1,
            }
        }
    });

    const db = st.s.getDB(dbName);
    const collection = db.getCollection(collName);

    //// Insert initial documents.

    const bulk = collection.initializeUnorderedBulkOp();
    for (let i = 0; i < kNumDocs; i++) {
        bulk.insert({x: i, y: i});
    }
    assert.commandWorked(bulk.execute());

    //// Execute currentOp before any query sampling.

    const startState = getCurrentOpAndServerStatus(st);
    assert.eq(startState.currentOp.mongos.length, 0);
    assert.eq(startState.currentOp.mongod.length, 0);

    //// Set initial state of counters.

    const initialState = {
        currentOp: {
            mongos: [{
                desc: "query analyzer",
                ns: nss,
                sampleRate: kSampleRate,
                sampledReadsCount: 0,
                sampledWritesCount: 0
            }],
            mongod: [{
                desc: "query analyzer",
                ns: nss,
                sampledReadsCount: 0,
                sampledWritesCount: 0,
                sampledReadsBytes: 0,
                sampledWritesBytes: 0
            }]
        },
        serverStatus: {
            mongos: {
                activeCollections: 1,
                totalCollections: 1,
                totalSampledReadsCount: 0,
                totalSampledWritesCount: 0
            },
            mongod: {
                totalCollections: 1,
                totalSampledReadsCount: 0,
                totalSampledWritesCount: 0,
                totalSampledReadsBytes: 0,
                totalSampledWritesBytes: 0
            }
        }
    };

    //// Start query analysis and send find queries.

    assert.commandWorked(
        st.s.adminCommand({configureQueryAnalyzer: nss, mode: "full", sampleRate: kSampleRate}));

    QuerySamplingUtil.waitForActiveSampling(st.s);

    //// Execute different kinds of queries and check counters.

    // Byte size of queries (fourth parameter in calls to doCommandAndAssertCounterState()) are
    // determined empirically.
    let state = runCommandAndAssertCurrentOpAndServerStatus(
        st, db, {find: collName, filter: {x: 1}}, opKindRead, initialState);
    state =
        runCommandAndAssertCurrentOpAndServerStatus(st, db, {count: collName}, opKindRead, state);
    state = runCommandAndAssertCurrentOpAndServerStatus(
        st, db, {update: collName, updates: [{q: {x: 1}, u: {updated: true}}]}, opKindWrite, state);
    state = runCommandAndAssertCurrentOpAndServerStatus(
        st,
        db,
        {findAndModify: collName, query: {updated: true}, update: {$set: {modified: 1}}},
        opKindWrite,
        state);
    state = runCommandAndAssertCurrentOpAndServerStatus(
        st, db, {delete: collName, deletes: [{q: {x: 1}, limit: 1}]}, opKindWrite, state);

    //// Stop query analysis and check counters
    assert.commandWorked(st.s.adminCommand({configureQueryAnalyzer: nss, mode: "off"}));

    let lastState;
    assert.soon(() => {
        lastState = getCurrentOpAndServerStatus(st);
        return 0 == lastState.serverStatus.mongos.activeCollections;
    });
    assert.soon(() => {
        lastState = getCurrentOpAndServerStatus(st);
        return 0 == lastState.currentOp.mongod.length && 0 == lastState.currentOp.mongos.length;
    });
    assert.eq(0, lastState.serverStatus.mongos.activeCollections);
    assert.eq(0, lastState.serverStatus.mongod.activeCollections);
    assert.eq(1, lastState.serverStatus.mongos.totalCollections);
    assert.eq(1, lastState.serverStatus.mongod.totalCollections);
    assert.eq(state.serverStatus.mongos.totalSampledReadsCount,
              lastState.serverStatus.mongos.totalSampledReadsCount);
    assert.eq(state.serverStatus.mongos.totalSampledWritesCount,
              lastState.serverStatus.mongos.totalSampledWritesCount);
    assert.eq(1, lastState.serverStatus.mongos.totalCollections);
    assert.eq(state.serverStatus.mongod.totalSampledReadsCount,
              lastState.serverStatus.mongod.totalSampledReadsCount);
    assert.eq(state.serverStatus.mongod.totalSampledWritesCount,
              lastState.serverStatus.mongod.totalSampledWritesCount);
    assert.eq(state.serverStatus.mongod.totalSampledReadsBytes,
              lastState.serverStatus.mongod.totalSampledReadsBytes);
    assert.eq(state.serverStatus.mongod.totalSampledWritesBytes,
              lastState.serverStatus.mongod.totalSampledWritesBytes);

    st.stop();
}

/**
 * Tests that mongos reports current information in currentOp even if its sampling rate is 0.
 * Specifically, a mongos's local sample rate will be 0 when all queries are being routed through
 * other mongos's. It should still report in currentOp the sampling of a collection with sample rate
 * of 0.
 */
function testCurrentOpZeroSampleRateMongos() {
    const st = new ShardingTest({
        shards: 1,
        mongos: [
            {
                setParameter: {
                    queryAnalysisSamplerConfigurationRefreshSecs: 1,
                }
            },
            {
                setParameter: {
                    // This failpoint will force this mongos to not count any queries or internal
                    // commands being processed through it. This will force its local sample rate
                    // to be exactly 0.
                    "failpoint.overwriteQueryAnalysisSamplerAvgLastCountToZero":
                        tojson({mode: "alwaysOn"}),
                    queryAnalysisSamplerConfigurationRefreshSecs: 1,
                }
            }
        ],
        rs: {
            nodes: 2,
            setParameter: {
                queryAnalysisWriterIntervalSecs: 1,
            }
        },
    });

    const mongos0Db = st.s0.getDB(dbName);
    const mongos1Db = st.s1.getDB(dbName);
    const mongos0Collection = mongos0Db.getCollection(collName);

    //// Insert initial documents.

    const bulk = mongos0Collection.initializeUnorderedBulkOp();
    for (let i = 0; i < kNumDocs; i++) {
        bulk.insert({x: i, y: i});
    }
    assert.commandWorked(bulk.execute());

    //// Turn on sampling and execute a bunch of queries through mongos0.

    assert.commandWorked(
        st.s.adminCommand({configureQueryAnalyzer: nss, mode: "full", sampleRate: 1000.0}));

    QuerySamplingUtil.waitForActiveSampling(st.s);

    let state;
    for (let i = -1; i++; i < kNumDocs * 2) {
        state = runCommandAndAssertCurrentOpAndServerStatus(
            st, mongos0Db, {find: collName, filter: {x: i % kNumDocs}}, opKindRead, state);
    }

    // Wait for at least queryAnalysisSamplerConfigurationRefreshSecs so that mongos1 can
    // refresh its sample rate.
    sleep(2000);

    const mongosCurrOp = assert
                             .commandWorked(mongos1Db.adminCommand({
                                 aggregate: 1,
                                 pipeline: [
                                     {$currentOp: {allUsers: true, localOps: true}},
                                     {$match: {desc: "query analyzer"}}
                                 ],
                                 cursor: {}
                             }))
                             .cursor.firstBatch[0];
    assert.eq(0.0, mongosCurrOp.sampleRate);
    assert.eq(0, mongosCurrOp.sampledReadsCount);
    assert.eq(0, mongosCurrOp.sampledWritesCount);

    st.stop();
}

{
    testCurrentOpAndServerStatusBasic();
    testCurrentOpZeroSampleRateMongos();
}
})();
