/**
 * A v2 change stream must never expose the resume token of an internally-swallowed control event
 * (e.g. namespacePlacementChanged) as its postBatchResumeToken (PBRT). Such a token is not
 * resumable: on replay the control event is consumed by the V2 topology-change stage and never
 * reaches DSCSEnsureResumeTokenPresent, so the next event is reported as having "surpassed" it,
 * producing ChangeStreamFatalError (code 280).
 *
 * On the shard, a control event is returned like a normal document, so the shard's own PBRT (and
 * hence the router's promised min sort key) can be the control event's event resume token. The
 * 'changeStreamKeepLastEventResumeTokenAsPBRT' fail point pins the shard's PBRT to the last returned
 * event's resume token, which removes the otherwise racy dependency on whether the shard has
 * already scanned past the control event. This test then verifies that such a token never surfaces
 * as a client-visible PBRT.
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

const kControlOpTypes = new Set(["moveChunk", "movePrimary", "namespacePlacementChanged"]);

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

describe("v2 change stream control-event resume token leak (collection-level)", function () {
    const dbName = jsTestName();
    const kCollName = "coll";
    const kFailPointName = "changeStreamKeepLastEventResumeTokenAsPBRT";

    let st;
    let db;
    let adminDb;

    before(function () {
        st = new ShardingTest({
            shards: 1,
            configShard: true,
            mongos: 1,
            // Do not advance resume token via the no-op oplog writer so that we can manufacture
            // a predictable resume token returned by the shard.
            rs: {nodes: 1, setParameter: {writePeriodicNoops: false}},
            other: {enableBalancer: false},
        });
        db = st.s.getDB(dbName);
        adminDb = st.s.getDB("admin");

        assert.commandWorked(
            adminDb.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
        );

        // Create and index the collection before opening the stream so that the placement change is
        // the last thing the stream observes.
        assert.commandWorked(db.createCollection(kCollName));
        assert.commandWorked(db[kCollName].createIndex({x: 1}));

        // Pin the shard's PBRT to the last returned event's resume token.
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

    // Resumes a collection-level stream from 'token' and returns the resulting command failure, or
    // null if the resume did not fail. A UUID-less control-event token is rejected at the aggregate
    // itself for a single-collection stream.
    function resumeFromToken(token) {
        // The fail point is only needed to produce the leaked token; disable it so that it cannot
        // interfere with the resumed stream.
        st.rs0.getPrimary().getDB("admin").runCommand({
            configureFailPoint: kFailPointName,
            mode: "off",
        });

        const res = db.runCommand({
            aggregate: kCollName,
            pipeline: [{$changeStream: {showExpandedEvents: true, resumeAfter: token}}],
            cursor: {batchSize: 1},
        });
        if (!res.ok) {
            return {code: res.code, codeName: res.codeName, errmsg: res.errmsg};
        }

        let cursorId = res.cursor.id;
        for (let i = 0; i < 50 && cursorId !== NumberLong(0); i++) {
            const gm = db.runCommand({getMore: cursorId, collection: kCollName, batchSize: 1});
            if (!gm.ok) {
                return {code: gm.code, codeName: gm.codeName, errmsg: gm.errmsg};
            }
            cursorId = gm.cursor.id;
        }
        return null;
    }

    it("does not expose a swallowed control event's resume token as PBRT", function () {
        const aggregateRes = assert.commandWorked(
            db.runCommand({
                aggregate: kCollName,
                pipeline: [{$changeStream: {showExpandedEvents: true}}],
                cursor: {batchSize: 0},
            }),
        );
        let cursorId = aggregateRes.cursor.id;
        assert.neq(NumberLong(0), cursorId, "expected an open cursor", {aggregateRes});

        // Emit a control event for the watched collection: shardCollection commits placement metadata
        // and notifies the owning shard with a namespacePlacementChanged event.
        assert.commandWorked(
            adminDb.adminCommand({shardCollection: dbName + "." + kCollName, key: {x: 1}}),
        );

        let observed = [];
        for (let i = 0; i < 60 && cursorId !== NumberLong(0); i++) {
            const getMoreRes = assert.commandWorked(
                db.runCommand({
                    getMore: cursorId,
                    collection: kCollName,
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
