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
            {"fieldName": "a", "data_type": "integer", "indexed": true},
            {"fieldName": "b", "data_type": "string", "indexed": true},
            {"fieldName": "c", "data_type": "array", "indexed": true},
            {"fieldName": "mixed", "data_type": "mixed", "indexed": true},
        ],
        "compound_indexes": [],
        "cardinality": 20
    };

    runCETestForCollection(db, collMeta);
});
})();
