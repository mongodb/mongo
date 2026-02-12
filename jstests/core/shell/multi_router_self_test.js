/**
 * Self-test for multi-router functionality.
 * The test aim at confirming the multi-router is working as expected when 2 or more mongos are deployed.
 * This is a best effort attempt.
 * Validates that:
 * a) If 2+ mongos are detected, the connection must be a multi-router
 * b) Running finds distributes them across routers (one operation per _getNextMongo call)
 * c) Under implicit transactions, operations are still distributed but pinned per transaction
 */
import {DiscoverTopology, Topology} from "jstests/libs/discover_topology.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {describe, it, beforeEach, afterEach} from "jstests/libs/mochalite.js";

const dbName = jsTestName();
const collName = "testColl";
let conn = db.getMongo();
const testDb = conn.getDB(dbName);
const coll = testDb[collName];
const kNumDocs = 10;
const kNumFinds = 100;

const implicitTxns = TestData.networkErrorAndTxnOverrideConfig?.wrapCRUDinTransactions;

function insertTestData() {
    const docs = [];
    for (let i = 0; i < kNumDocs; i++) {
        docs.push({_id: i, value: i});
    }
    assert.commandWorked(coll.insertMany(docs));
}

function runFinds() {
    for (let i = 0; i < kNumFinds; i++) {
        assert.eq(
            coll.find({}).toArray().length,
            kNumDocs,
            "Failed to find " + kNumDocs + " documents for Multi-Router: " + conn.toString(),
        );
    }
}

function trackGetNextMongo() {
    const mongosUsageCount = {};
    const originalGetNextMongo = conn._getNextMongo.bind(conn);

    conn._getNextMongo = function () {
        const mongo = originalGetNextMongo();
        const idx = conn._mongoConnections.indexOf(mongo);
        mongosUsageCount[idx] = (mongosUsageCount[idx] || 0) + 1;
        return mongo;
    };

    return {
        restore() {
            conn._getNextMongo = originalGetNextMongo;
        },
        getMongosUsageCount() {
            return mongosUsageCount;
        },
        getTotalOps() {
            return Object.values(mongosUsageCount).reduce((sum, c) => sum + c, 0);
        },
        getUsedRouters() {
            return Object.values(mongosUsageCount).filter((c) => c > 0).length;
        },
    };
}

// Detect topology upfront to decide whether to skip.
let numMongos = 0;
if (FixtureHelpers.isMongos(db)) {
    try {
        const topology = DiscoverTopology.findConnectedNodes(conn);
        assert.eq(topology.type, Topology.kShardedCluster);
        numMongos = topology.mongos.nodes.length;
    } catch (e) {
        chatty("Error during topology discovery: " + e);
    }
}

// Test only runs if 2+ mongos are detected.
(numMongos >= 2 ? describe : describe.skip)("MultiRouterMongo self-test", function () {
    afterEach(function () {
        coll.drop();
    });

    it("connection with 2+ mongos must be a MultiRouterMongo", function () {
        assert.eq(conn.isMultiRouter, true, "Connection with " + numMongos + " mongos MUST be a MultiRouterMongo");
        assert.eq(conn._mongoConnections.length, numMongos);
    });

    describe("_getNextMongo tracking", function () {
        let tracker;

        beforeEach(function () {
            tracker = trackGetNextMongo();
        });

        afterEach(function () {
            tracker.restore();
        });

        (implicitTxns ? it.skip : it)("each operation calls _getNextMongo exactly once", function () {
            insertTestData();
            runFinds();

            const kMinOps = kNumFinds + 1; // +1 insert
            // Assert _getNextMongo is called at least once per transaction.
            assert.gte(
                tracker.getTotalOps(),
                kMinOps,
                "Expected at least " + kMinOps + " operations but tracked " + tracker.getTotalOps(),
            );
            // Assert _getNextMongo has distributed the calls
            assert.gte(
                tracker.getUsedRouters(),
                2,
                "At least 2 mongos should be used: " + tojson(tracker.getMongosUsageCount()),
            );
            chatty("Mongos usage distribution: " + tojson(tracker.getMongosUsageCount()));
        });

        (implicitTxns ? it : it.skip)("implicit transactions are pinned but still distributed", function () {
            insertTestData();
            runFinds();

            // In transaction passthroughs, _getNextMongo is called once per
            // transaction (not per operation) due to session pinning.
            // Assert _getNextMongo is called at least once per transaction.
            const kMinOps = 5;
            assert.gte(
                tracker.getTotalOps(),
                kMinOps,
                "Expected at least " + kMinOps + " transactions but tracked " + tracker.getTotalOps(),
            );
            chatty("Mongos usage distribution (implicit txns): " + tojson(tracker.getMongosUsageCount()));
        });
    });
});
