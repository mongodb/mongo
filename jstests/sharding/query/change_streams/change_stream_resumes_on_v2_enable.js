/**
 * Verifies that change streams opened with the IFR flag 'featureFlagChangeStreamReaderV2' disabled
 * produce events and resume tokens that remain identical after the flag is re-enabled across a
 * cluster restart. The setup creates a mix of inserts, updates, and deletes across two shards, records
 * the resulting v1 events with their resume tokens for collection, database, and whole-cluster change
 * streams, and then restarts the cluster with the v2 reader enabled. The test cases re-open the stream
 * from the initial point, resume from each recorded resume token, and resume after the last event,
 * asserting that the v2 path returns exactly the same events and tokens in each change stream mode.
 *
 * @tags: [
 *   assumes_balancer_off,
 *   does_not_support_stepdowns,
 *   featureFlagChangeStreamPreciseShardTargeting,
 *   requires_fcv_90,
 *   requires_persistence,
 *   requires_sharding,
 *   uses_change_streams,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ChangeStreamTest, ChangeStreamWatchMode} from "jstests/libs/query/change_stream_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("change-stream v2 re-enable", function () {
    const ifrFlagName = "featureFlagChangeStreamReaderV2";
    const dbName = jsTestName();
    const collName = "coll";

    let st;
    let mongosSetParams;
    let mongodSetParams;
    let startTimestamp;
    let modeExpectedEvents;
    let db;
    let coll;

    before(function () {
        mongosSetParams = {
            writePeriodicNoops: true,
            periodicNoopIntervalSecs: 1,
            // The v2 targeter's init logs are debug-3 messages; raise query verbosity on mongos so
            // we can observe any resumed stream opening on the v2 path.
            logComponentVerbosity: tojson({query: {verbosity: 3}}),
            [ifrFlagName]: false,
        };
        mongodSetParams = {
            [ifrFlagName]: false,
        };

        // Start the cluster with the v2 IFR flag disabled. Change streams will use v1 targeting.
        st = new ShardingTest({
            shards: 2,
            mongos: 1,
            rs: {nodes: 1},
            config: 1,
            mongosOptions: {setParameter: mongosSetParams},
            rsOptions: {setParameter: mongodSetParams},
            configOptions: {setParameter: mongodSetParams},
        });

        db = st.s.getDB(dbName);

        // Confirm the IFR flag is disabled on mongos.
        const getParamRes = assert.commandWorked(
            db.adminCommand({getParameter: 1, [ifrFlagName]: 1}),
        );
        assert.eq(getParamRes[ifrFlagName].value, false, {getParamRes});

        // Enable sharding and split the collection so low _ids live on shard 0 and high _ids on
        // shard 1.
        assert.commandWorked(
            db.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
        );
        assert.commandWorked(
            db.adminCommand({shardCollection: `${dbName}.${collName}`, key: {_id: 1}}),
        );
        assert.commandWorked(db.adminCommand({split: `${dbName}.${collName}`, middle: {_id: 5}}));
        assert.commandWorked(st.moveChunk(`${dbName}.${collName}`, {_id: 10}, st.shard1.shardName));

        // Record the start timestamp before generating any change events.
        startTimestamp = assert.commandWorked(db.adminCommand({hello: 1})).$clusterTime.clusterTime;

        // Generate a mixture of inserts, updates, and deletes across both shards.
        coll = db[collName];
        assert.commandWorked(coll.insert({_id: 1, v: "a"}));
        assert.commandWorked(coll.insert({_id: 10, v: "b"}));
        assert.commandWorked(coll.update({_id: 1}, {$set: {v: "a-updated"}}));
        assert.commandWorked(coll.update({_id: 10}, {$set: {v: "b-updated"}}));
        assert.commandWorked(coll.remove({_id: 1}, {justOne: true}));
        assert.commandWorked(coll.insert({_id: 2, v: "c"}));
        assert.commandWorked(coll.remove({_id: 10}, {justOne: true}));
        assert.commandWorked(coll.insert({_id: 11, v: "d"}));

        // Capture the v1 events for each change stream mode.
        modeExpectedEvents = {};
        const expectedTypes = [
            "insert",
            "insert",
            "update",
            "update",
            "delete",
            "insert",
            "delete",
            "insert",
        ];

        for (const mode of Object.keys(ChangeStreamWatchMode)) {
            const csTest = new ChangeStreamTest(db);
            const cursor = csTest.getChangeStream({
                watchMode: mode,
                coll,
                startAtOperationTime: startTimestamp,
            });
            modeExpectedEvents[mode] = csTest.getNextChanges(cursor, expectedTypes.length);

            // Sanity-check the captured events: order should match the operation sequence above.
            assert.eq(modeExpectedEvents[mode].length, expectedTypes.length, {
                mode,
                events: modeExpectedEvents[mode],
            });
            modeExpectedEvents[mode].forEach((event, i) => {
                assert.eq(event.operationType, expectedTypes[i], {mode, event, i});
            });
        }

        // Re-enable the IFR flag across all cluster components and restart them.
        mongosSetParams[ifrFlagName] = true;
        mongodSetParams[ifrFlagName] = true;

        st.stopAllShards({startClean: false}, /* forRestart */ true);
        st.restartAllShards({setParameter: mongodSetParams});

        st.stopAllConfigServers({startClean: false}, /* forRestart */ true);
        st.restartAllConfigServers({setParameter: mongodSetParams});

        st.restartMongos(0, {
            restart: true,
            setParameter: mongosSetParams,
        });

        // Refresh the db reference after the mongos restart and confirm the flag is enabled.
        db = st.s.getDB(dbName);
        const getParamResAfter = assert.commandWorked(
            db.adminCommand({getParameter: 1, [ifrFlagName]: 1}),
        );
        assert.eq(getParamResAfter[ifrFlagName].value, true, {getParamResAfter});
    });

    after(function () {
        st.stop();
    });

    it("replays all events from the initial point after re-enabling v2", function () {
        for (const batchSize of [1, 0]) {
            for (const mode of Object.keys(ChangeStreamWatchMode)) {
                const expectedEvents = modeExpectedEvents[mode];

                const csTest = new ChangeStreamTest(db);
                const cursor = csTest.getChangeStream({
                    watchMode: mode,
                    coll,
                    startAtOperationTime: startTimestamp,
                    batchSize,
                });
                csTest.assertNextChangesEqual({
                    cursor,
                    expectedEvents,
                    expectedNumChanges: expectedEvents.length,
                });
                csTest.cleanUp();
            }
        }
    });

    it("resumes from each recorded event after re-enabling v2", function () {
        for (const batchSize of [1, 0]) {
            for (const mode of Object.keys(ChangeStreamWatchMode)) {
                const expectedEvents = modeExpectedEvents[mode];
                for (let i = 0; i < expectedEvents.length; i++) {
                    const resumeToken = expectedEvents[i]._id;
                    const expectedTail = expectedEvents.slice(i + 1);

                    const csTest = new ChangeStreamTest(db);
                    const cursor = csTest.getChangeStream({
                        watchMode: mode,
                        coll,
                        resumeAfter: resumeToken,
                        batchSize,
                    });
                    csTest.assertNextChangesEqual({
                        cursor,
                        expectedChanges: expectedTail,
                        expectedNumChanges: expectedTail.length,
                    });
                    csTest.cleanUp();
                }
            }
        }
    });

    it("resumes after the last event and observes no further changes", function () {
        for (const batchSize of [1, 0]) {
            for (const mode of Object.keys(ChangeStreamWatchMode)) {
                const expectedEvents = modeExpectedEvents[mode];
                const lastResumeToken = expectedEvents[expectedEvents.length - 1]._id;

                const csTest = new ChangeStreamTest(db);
                const cursor = csTest.getChangeStream({
                    watchMode: mode,
                    coll,
                    resumeAfter: lastResumeToken,
                    batchSize,
                });
                csTest.assertNoChange(cursor);
                csTest.cleanUp();
            }
        }
    });
});
