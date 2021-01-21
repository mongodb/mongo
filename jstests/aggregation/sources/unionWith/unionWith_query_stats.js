/**
 * Tests that the queryExecutor stats are correctly returned when $unionWith is performed on
 * collections.
 *
 * @tags: [
 *     assumes_unsharded_collection,
 *     do_not_wrap_aggregations_in_facets,
 *     assumes_read_preference_unchanged,
 *     assumes_read_concern_unchanged,
 *     assumes_against_mongod_not_mongos
 * ]
 */
(function() {
"use strict";

const testDB = db.getSiblingDB("union_with_query_stats");
testDB.dropDatabase();

const collData = [
    ["firstCol", Array.from({length: 10}, (_, i) => ({_id: i, "aField": i}))],
    ["secondColl", Array.from({length: 20}, (_, i) => ({_id: i, "aField": i}))],
    ["thirdColl", Array.from({length: 30}, (_, i) => ({_id: i, "aField": i}))],
    ["forthColl", Array.from({length: 40}, (_, i) => ({_id: i, "aField": i}))]
];

const colls = Array.from(collData, elem => testDB.getCollection(elem[0]));

for (let idx = 0; idx < collData.length; idx++) {
    const coll = colls[idx];
    const collDocs = collData[idx][1];
    assert.commandWorked(coll.insert(collDocs));
}

(function testUnionWithQueryStats() {
    // Collect the previous scannedObjects before running aggregation. This value will be
    // subtracted from the current scannedObjects and will prevent test case from failing if it is
    // run on existing mongoD instance.
    const prevScannedObjects = testDB.serverStatus().metrics.queryExecutor.scannedObjects;

    const pipeline = [
        {$unionWith: {coll: collData[1][0]}},
        {$unionWith: {coll: collData[2][0], pipeline: [{$unionWith: {coll: collData[3][0]}}]}},
        {$sort: {_id: 1}}
    ];

    const output = colls[0].aggregate(pipeline).toArray();

    // Concatenate and sort arrays by '_id'.
    let expectedOutput = [].concat(collData[0][1], collData[1][1], collData[2][1], collData[3][1])
                             .sort((elem1, elem2) => elem1._id - elem2._id);

    assert.eq(output, expectedOutput);
    assert.eq(expectedOutput.length,
              testDB.serverStatus().metrics.queryExecutor.scannedObjects - prevScannedObjects);
})();
})();
