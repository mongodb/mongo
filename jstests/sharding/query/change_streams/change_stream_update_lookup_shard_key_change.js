/**
 * Verifies the update event's fullDocument for an update that changes the shard key, with
 * fullDocument: "updateLookup", comparing the optimized updateLookup feature flag on and off.
 *
 * Expected: with the flag off (released 9.0.x), the legacy lookup matches the event's documentKey,
 * which holds the OLD shard key value, so it misses and fullDocument is null. With the flag on,
 * the optimized lookup routes by the old shard key and reads by _id only, so it finds the document
 * (with its new shard key values).
 *
 * @tags: [
 *   requires_fcv_90,
 *   featureFlagChangeStreamOptimizedUpdateLookup,
 *   uses_change_streams,
 *   assumes_balancer_off,
 * ]
 */
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {withChangeStreamTest} from "jstests/libs/query/change_stream_util.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {withSession} from "jstests/libs/query/change_stream_util.js";
import {
    assertDropCollection,
    assertCreateCollection,
} from "jstests/libs/collection_drop_recreate.js";

const flagName = "featureFlagChangeStreamOptimizedUpdateLookup";
const flagOn = {[flagName]: true};
const flagOff = {[flagName]: false};

describe("updateLookup after a shard-key change", function () {
    let st;
    let testDB;
    let coll;
    const collName = "testColl";

    before(function () {
        // Periodic noops keep cluster time advancing so update events surface promptly.
        st = new ShardingTest({
            shards: 2,
            rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}},
        });
        testDB = st.s.getDB(jsTestName());

        assert.commandWorked(
            testDB.adminCommand({
                enableSharding: testDB.getName(),
                primaryShard: st.shard0.shardName,
            }),
        );
    });

    // Start from a pristine sharded collection whose primary is shard0; the single initial chunk
    // covers [MinKey, MaxKey), so the shard-key change below stays same-shard and the stream emits
    // an update event (not delete+insert).
    beforeEach(function () {
        coll = assertCreateCollection(testDB, collName);
        assert.commandWorked(
            testDB.adminCommand({
                shardCollection: coll.getFullName(),
                key: {a: 1},
            }),
        );
        assert.commandWorked(coll.insertOne({_id: 1, a: 1}));
    });

    afterEach(function () {
        assertDropCollection(testDB, collName);
    });

    after(function () {
        st.stop();
    });

    // Opens an updateLookup stream, performs a control update that does not touch the shard key,
    // then a shard-key change a:1 -> a:2 in a transaction (shard-key updates must run in a
    // multi-statement transaction or with retryWrites: true), and returns the update event's
    // fullDocument.
    function runUpdateLookupScenario() {
        return withChangeStreamTest(testDB, (cst) => {
            const cursor = cst.startWatchingChanges({
                pipeline: [{$changeStream: {fullDocument: "updateLookup"}}],
                collection: coll.getName(),
            });

            assert.commandWorked(coll.updateOne({_id: 1}, {$set: {b: 1}}));
            const ctrl = cst.getOneChange(cursor);
            assert.eq(ctrl.operationType, "update", {ctrl});
            assert.neq(null, ctrl.fullDocument, {ctrl});

            withSession(testDB.getMongo(), (session) => {
                const sessColl = session.getDatabase(testDB.getName()).getCollection(collName);
                session.startTransaction();
                assert.commandWorked(sessColl.updateOne({_id: 1}, {$set: {a: 2}}));
                session.commitTransaction();
            });

            const event = cst.getOneChange(cursor);
            assert.eq(event.operationType, "update", {event});
            assert.eq(event.documentKey.a, 1, {event});
            jsTest.log.info("update event after shard-key change", {event});
            return event.fullDocument;
        });
    }

    it("optimized flag on: fullDocument present", function () {
        const fullDocument = runWithParamsAllNonConfigNodes(
            testDB,
            flagOn,
            runUpdateLookupScenario,
        );
        assert.eq(fullDocument, {_id: 1, a: 2, b: 1}, "expected fullDocument to be found", {
            fullDocument,
        });
    });

    it("optimized flag off: fullDocument null", function () {
        const fullDocument = runWithParamsAllNonConfigNodes(
            testDB,
            flagOff,
            runUpdateLookupScenario,
        );
        assert.eq(
            fullDocument,
            null,
            "expected fullDocument not to be found as lookup is performed with an old documentKey",
            {fullDocument},
        );
    });
});
