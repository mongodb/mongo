/**
 * An all-databases v2 change stream on a sharded cluster reads from both the data shards and the
 * config server. A v2 stream must never expose the resume token of an internally-swallowed control
 * event (e.g. namespacePlacementChanged) as its postBatchResumeToken (PBRT): on replay the control
 * event is consumed by the V2 topology-change stage and never reaches DSCSEnsureResumeTokenPresent,
 * so the next event is reported as having "surpassed" it, producing ChangeStreamFatalError (280).
 *
 * The 'changeStreamKeepLastEventResumeTokenAsPBRT' fail point pins each change stream cursor's PBRT
 * to the resume token of the last event it returned, removing the otherwise racy dependency on
 * whether the cursor has already scanned past its last returned event.
 *
 * This variant also exercises the config-server cursor: because the router adopts the *minimum*
 * promised sort key across all cursors, a 'databaseCreated' control event on the config server is
 * generated after the data shard's 'namespacePlacementChanged' control event. That keeps the config
 * server's promise after the data shard's promise, so the data shard's (non-resumable) control-event
 * token would become the client-visible PBRT without the fix.
 *
 * @tags: [
 *   assumes_balancer_off,
 *   does_not_support_stepdowns,
 *   featureFlagChangeStreamPreciseShardTargeting,
 *   featureFlagChangeStreamReaderV2,
 *   requires_fcv_90,
 *   requires_sharding,
 *   uses_change_streams,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const kControlOpTypes = new Set([
    "moveChunk",
    "movePrimary",
    "namespacePlacementChanged",
    "insert", // databaseCreated is an insert into config.databases.
]);

function decodeTokenSummary(token) {
    const decoded = decodeResumeToken(token);
    const eventIdentifier = decoded.eventIdentifier || {};
    return {
        clusterTime: decoded.clusterTime,
        tokenType: decoded.tokenType,
        operationType: eventIdentifier.operationType,
    };
}

function isControlEventToken(token) {
    const summary = decodeTokenSummary(token);
    return summary.tokenType === eventResumeTokenType && kControlOpTypes.has(summary.operationType);
}

describe("v2 all-databases change stream control-event resume token leak", function () {
    const dbName = jsTestName();
    const kFailPointName = "changeStreamKeepLastEventResumeTokenAsPBRT";
    const kCollName = "coll";

    let st;
    let db;
    let adminDb;

    before(function () {
        st = new ShardingTest({
            shards: 1,
            configShard: true,
            mongos: 1,
            // Do not advance resume token via the no-op oplog writer so that we can manufacture a
            // predictable resume token returned by the shard.
            rs: {nodes: 1, setParameter: {writePeriodicNoops: false}},
            other: {enableBalancer: false},
        });
        db = st.s.getDB(dbName);
        adminDb = st.s.getDB("admin");

        assert.commandWorked(
            adminDb.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
        );

        // Create and index the collection before opening the stream so that the placement change is
        // the last data-shard event the stream observes.
        assert.commandWorked(db.createCollection(kCollName));
        assert.commandWorked(db[kCollName].createIndex({x: 1}));

        // Pin both the data-shard and the config-server PBRT to the last returned event's resume
        // token.
        assert.commandWorked(
            st.rs0.getPrimary().getDB("admin").runCommand({
                configureFailPoint: kFailPointName,
                mode: "alwaysOn",
            }),
        );
    });

    after(function () {
        try {
            st.rs0.getPrimary().getDB("admin").runCommand({
                configureFailPoint: kFailPointName,
                mode: "off",
            });
        } catch (e) {
            // Ignore cleanup errors (e.g. the primary changed).
        }
        st.stop();
    });

    // Resumes an all-databases stream from 'token' and returns the resulting command failure, or
    // null if the resume did not fail. An all-databases stream accepts a UUID-less token, so a
    // subsequent event is written to make the resumed stream surpass (and reject) the swallowed
    // control-event token.
    function resumeFromToken(token, collectionName) {
        // The fail point is only needed to produce the leaked token; disable it so that it cannot
        // interfere with the resumed stream.
        st.rs0.getPrimary().getDB("admin").runCommand({
            configureFailPoint: kFailPointName,
            mode: "off",
        });

        // Deliver an event after the swallowed control event so that the resumed stream surpasses
        // the resume token.
        assert.commandWorked(db[kCollName].insert({x: 2}));

        const res = adminDb.runCommand({
            aggregate: 1,
            pipeline: [
                {
                    $changeStream: {
                        allChangesForCluster: true,
                        showExpandedEvents: true,
                        resumeAfter: token,
                    },
                },
            ],
            cursor: {batchSize: 1},
        });
        if (!res.ok) {
            return {code: res.code, codeName: res.codeName, errmsg: res.errmsg};
        }

        let cursorId = res.cursor.id;
        for (let i = 0; i < 60 && cursorId !== NumberLong(0); i++) {
            const gm = adminDb.runCommand({
                getMore: cursorId,
                collection: collectionName,
                batchSize: 1,
                maxTimeMS: 1000,
            });
            if (!gm.ok) {
                return {code: gm.code, codeName: gm.codeName, errmsg: gm.errmsg};
            }
            cursorId = gm.cursor.id;
        }
        return null;
    }

    it("does not expose a swallowed control event's resume token as PBRT", function () {
        const aggregateRes = assert.commandWorked(
            adminDb.runCommand({
                aggregate: 1,
                pipeline: [{$changeStream: {allChangesForCluster: true, showExpandedEvents: true}}],
                cursor: {batchSize: 0},
            }),
        );
        const cursorNs = aggregateRes.cursor.ns;
        const collectionName = cursorNs.substring(cursorNs.indexOf(".") + 1);
        let cursorId = aggregateRes.cursor.id;
        assert.neq(NumberLong(0), cursorId, "expected an open cursor", {aggregateRes});

        // Emit a data-shard control event for the watched collection.
        assert.commandWorked(
            adminDb.adminCommand({shardCollection: dbName + "." + kCollName, key: {x: 1}}),
        );
        // Emit a config-server control event afterwards, so that the config server's promise sorts
        // after the data shard's control-event token.
        assert.commandWorked(adminDb.adminCommand({enableSharding: "otherDb_" + jsTestName()}));

        let observed = [];
        for (let i = 0; i < 60 && cursorId !== NumberLong(0); i++) {
            const getMoreRes = assert.commandWorked(
                adminDb.runCommand({
                    getMore: cursorId,
                    collection: collectionName,
                    batchSize: 1,
                    maxTimeMS: 500,
                }),
            );
            cursorId = getMoreRes.cursor.id;

            const pbrt = getMoreRes.cursor.postBatchResumeToken;
            if (pbrt) {
                const summary = decodeTokenSummary(pbrt);
                observed.push(summary);
                assert(!isControlEventToken(pbrt), "observed control-event resume token as PBRT", {
                    summary,
                    pbrt,
                    observed,
                });
            }
        }
    });
});
