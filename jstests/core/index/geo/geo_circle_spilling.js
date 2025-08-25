// Test spilling in geo near. Data has a lot of points on a circle, which forces NearStage to buffer
// all of them.
// @tags: [
//   requires_fcv_83,
//   requires_getmore,
//   requires_persistence,
//   featureFlagExtendedAutoSpilling,
//   assumes_unsharded_collection,
//   # Insertions make transactions too big
//   does_not_support_transactions
// ]

import {getExecutionStages, getPlanStages} from "jstests/libs/query/analyze_plan.js";

const string1MB = "A".repeat(1024 * 1024);
const docCount = 200;

const nearPredicate = {
    geo: {$near: {$geometry: {type: "Point", coordinates: [0, 0]}}},
};

function assertSpillingAndAllDocumentsReturned(coll) {
    const explain = coll.find(nearPredicate).explain("executionStats");
    jsTest.log.info("Running query", {explain});
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

{
    // Check regular collection - spilling buffer will spill
    const coll = db.geo_circle_spilling;
    coll.drop();
    coll.createIndex({geo: "2dsphere"});

    insertDocuments(coll, (i) => {
        return {
            index: i,
            geo: {type: "Point", coordinates: [Math.cos(i), Math.sin(i)]},
            payload: string1MB,
        };
    });

    assertSpillingAndAllDocumentsReturned(coll);
}

{
    // Check clustered collection - RecordIdDeduplicator should also spill
    db.runCommand({
        create: "geo_circle_spilling_clustered",
        clusteredIndex: {"key": {_id: 1}, "unique": true, "name": "large string clustered key"},
    });
    const clusteredColl = db.geo_circle_spilling_clustered;
    clusteredColl.createIndex({geo: "2dsphere"});

    insertDocuments(clusteredColl, (i) => {
        return {
            _id: i + string1MB,
            index: i,
            geo: {type: "Point", coordinates: [Math.cos(i), Math.sin(i)]},
        };
    });

    assertSpillingAndAllDocumentsReturned(clusteredColl);
}
