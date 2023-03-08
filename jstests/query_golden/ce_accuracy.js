/**
 * Tests for cardinality estimation accuracy.
 * @tags: [
 *   requires_cqf,
 * ]
 */

(function() {

load("jstests/query_golden/libs/ce_data.js");
load("jstests/query_golden/libs/run_queries_ce.js");

runHistogramsTest(function() {
    const coll = db.ce_data_20;
    coll.drop();

    jsTestLog("Populating collection");
    assert.commandWorked(coll.insertMany(getCEDocs()));
    assert.commandWorked(coll.insertMany(getCEDocs1()));
    const collSize = coll.find().itcount();
    print(`Collection count: ${collSize}\n`);

    const collMeta = {
        "collectionName": "ce_data_20",
        "fields": [
            {"fieldName": "a", "dataType": "integer", "indexed": true},
            {"fieldName": "b", "dataType": "string", "indexed": true},
            {"fieldName": "c_int", "dataType": "array", "indexed": true},
            {"fieldName": "mixed", "dataType": "mixed", "indexed": true},
        ],
        "compound_indexes": [],
        "cardinality": 20
    };

    // Flag to show more information for debugging purposes:
    // - adds execution of sampling CE strategy;
    // - prints plan skeleton.
    const ceDebugFlag = false;
    runCETestForCollection(db, collMeta, 4, ceDebugFlag);
});
})();
