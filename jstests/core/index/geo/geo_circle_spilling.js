// Test spilling in geo near. Data has a lot of points on a circle, which forces NearStage to buffer
// all of them.
// @tags: [
//   requires_fcv_83,
//   requires_getmore,
//   requires_persistence,
//   assumes_unsharded_collection,
//   # If the collection uuid changes while the find command in this test is running, the command
//   # is expected to fail with QueryPlanKilled.
//   assumes_stable_collection_uuid,
//   # Insertions make transactions too big
//   does_not_support_transactions
// ]

import {getExecutionStages, getPlanStages} from "jstests/libs/query/analyze_plan.js";
import {add2dsphereVersionIfNeeded} from "jstests/libs/query/geo_index_version_helpers.js";
import {setParameterOnAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

// TODO(SERVER-103530) : Remove multiversion check when 9.0 becomes last-continuous.
const isMultiversion =
    Boolean(jsTest.options().useRandomBinVersionsWithinReplicaSet) || Boolean(TestData.multiversionBinVersion);

// Use small strings and a low memory limit to avoid overwhelming test machines while still
// triggering spilling and memory limit enforcement.
const kMemoryLimitBytes = 1024 * 1024; // 1MB
const string10KB = "A".repeat(10 * 1024);
const string1KB = "B".repeat(1024);
const docCount = 200;

const nearPredicate = {
    geo: {$near: {$geometry: {type: "Point", coordinates: [0, 0]}}},
};

function assertSpillingAndAllDocumentsReturned(coll) {
    jsTest.log.info("Running query", nearPredicate);

    assert.throwsWithCode(
        () => coll.find(nearPredicate).allowDiskUse(false).itcount(),
        ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed,
    );

    const explain = coll.find(nearPredicate).explain("executionStats");
    let assertedSpilling = false;
    for (let stages of getExecutionStages(explain)) {
        for (let stage of getPlanStages(stages, "GEO_NEAR_2DSPHERE")) {
            assertedSpilling = true;
            assert.eq(stage.usedDisk, true);
            assert.gte(stage.spills, 1);
        }
    }
    assert(assertedSpilling, "No GEO_NEAR_2DSPHERE stages found: " + tojson(explain));

    const indexes = [];
    const cursor = coll.find(nearPredicate);
    while (cursor.hasNext()) {
        indexes.push(cursor.next().index);
    }
    assert.eq(docCount, indexes.length);
    indexes.sort((a, b) => a - b);
    for (let i = 0; i < docCount; ++i) {
        assert.eq(indexes[i], i, indexes);
    }
}

function assertNearStageThrowsMemoryLimit(coll) {
    jsTest.log.info("Running query", nearPredicate);
    // In suites using the multiplanner, explain("queryPlanner") may itself fail with 12227900
    // because the multiplanner executes all candidate plans during selection. If so, the error
    // proves the GEO_NEAR_2DSPHERE stage was reached. In other suites (e.g. CBR heuristic),
    // explain succeeds and we verify the stage in the winning plan before running the query.
    try {
        const explain = coll.find(nearPredicate).explain("queryPlanner");
        const foundStages = getPlanStages(explain.queryPlanner.winningPlan, "GEO_NEAR_2DSPHERE");
        assert.gt(foundStages.length, 0, "No GEO_NEAR_2DSPHERE stages found: " + tojson(explain));
    } catch (e) {
        assert.eq(e.code, 12227900, "Unexpected error from explain: " + tojson(e));
        return;
    }
    assert.throwsWithCode(() => coll.find(nearPredicate).toArray(), 12227900);
}

function insertDocuments(coll, generateDocument) {
    let docs = [];
    for (let i = 0; i < docCount; ++i) {
        docs.push(generateDocument(i));
        if (docs.length >= 10) {
            assert.commandWorked(coll.insertMany(docs));
            docs = [];
        }
    }
    if (docs.length > 0) {
        assert.commandWorked(coll.insertMany(docs));
    }
}

const originalMemoryLimit = assert.commandWorked(
    db.adminCommand({getParameter: 1, internalNearStageMaxMemoryBytes: 1}),
).internalNearStageMaxMemoryBytes;

try {
    setParameterOnAllNonConfigNodes(db.getMongo(), "internalNearStageMaxMemoryBytes", kMemoryLimitBytes);

    {
        // Check regular collection - spilling buffer will spill
        const coll = db.geo_circle_spilling;
        coll.drop();
        coll.createIndex({geo: "2dsphere"}, add2dsphereVersionIfNeeded());

        insertDocuments(coll, (i) => {
            return {
                index: i,
                geo: {type: "Point", coordinates: [Math.cos(i), Math.sin(i)]},
                payload: string10KB,
            };
        });

        assertSpillingAndAllDocumentsReturned(coll);
    }

    {
        // Check clustered collection with small RecordIds (1KB _id) and large payload (10KB).
        // _seenDocuments holds 200 x 1KB RecordIds (~200KB), within the 1MB limit, so spilling
        // the result buffer is sufficient and the query succeeds.
        const clusteredColl = db.geo_circle_spilling_clustered_large_payload;
        clusteredColl.drop();
        assert.commandWorked(
            db.runCommand({
                create: clusteredColl.getName(),
                clusteredIndex: {"key": {_id: 1}, "unique": true, "name": "small string clustered key"},
            }),
        );
        clusteredColl.createIndex({geo: "2dsphere"}, add2dsphereVersionIfNeeded());

        insertDocuments(clusteredColl, (i) => {
            return {
                _id: i + string1KB,
                index: i,
                geo: {type: "Point", coordinates: [Math.cos(i), Math.sin(i)]},
                payload: string10KB,
            };
        });

        assertSpillingAndAllDocumentsReturned(clusteredColl);
    }

    if (!isMultiversion) {
        // Check clustered collection with big RecordIds (10KB _id) and no payload.
        // _seenDocuments holds 200 x 10KB RecordIds (~2MB), exceeding the 1MB limit even after
        // spilling the result buffer, so the query is expected to fail.
        const clusteredColl = db.geo_circle_spilling_clustered;
        clusteredColl.drop();
        assert.commandWorked(
            db.runCommand({
                create: clusteredColl.getName(),
                clusteredIndex: {"key": {_id: 1}, "unique": true, "name": "large string clustered key"},
            }),
        );
        clusteredColl.createIndex({geo: "2dsphere"}, add2dsphereVersionIfNeeded());

        insertDocuments(clusteredColl, (i) => {
            return {
                _id: i + string10KB,
                index: i,
                geo: {type: "Point", coordinates: [Math.cos(i), Math.sin(i)]},
            };
        });

        assertNearStageThrowsMemoryLimit(clusteredColl);
    }
} finally {
    setParameterOnAllNonConfigNodes(db.getMongo(), "internalNearStageMaxMemoryBytes", originalMemoryLimit);
}
