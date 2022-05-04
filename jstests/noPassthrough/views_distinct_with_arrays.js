/**
 * Check that various distinct commands return the same results regardless of whether the collection
 * is a view.
 * @tags: [
 *   requires_fcv_60,
 *   assumes_unsharded_collection,
 *   # Explain of a resolved view must be executed by mongos.
 *   directly_against_shardsvrs_incompatible,
 *   # Changing FCV blocks migration
 *   tenant_migration_incompatible
 * ]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // arrayEq

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");

const viewsDB = conn.getDB(jsTestName());
viewsDB.dropDatabase();
const viewCollName = "distinctViewColl";
const baseCollName = "distinctBaseColl";
function getDocument(id, value) {
    return {
        _id: id,
        x: value,
        nested: {x: value, nested: {x: value}, array: [value]},
        array: [value],
        documentArray: [{x: value}, {x: value << 2}],
        doubleArray: [[{x: value}], [{x: value << 2}]],
        numeric: {"0": value},
        arrayNumeric: [{"0": value}, {"0": value << 2}],
        "0": {x: value},
        "1": [{"0": value}, {"0": value << 2}],
        "2": [[{"0": value}], [{"0": value << 2}]],
    };
}

const coll = viewsDB[baseCollName];
coll.drop();
coll.insertMany([getDocument(1, 1), getDocument(2, 2), getDocument(3, 3), getDocument(4, 1)]);

const viewColl = viewsDB[viewCollName];
viewColl.drop();
assert.commandWorked(viewsDB.createView(viewCollName, baseCollName, []));

function buildComparisonString(query, collRes, viewRes) {
    const collExplain = coll.explain().distinct(query);
    const viewExplain = viewColl.explain().distinct(query);
    return "Distinct: '" + query + "'\nCollection returned:\n" + tojson(collRes) + "\nExplain:\n" +
        tojson(collExplain) + "\nView returned:\n" + tojson(viewRes) + "\nExplain:\n" +
        tojson(viewExplain);
}
function compareDistinctResults(query) {
    const collResult = coll.distinct(query);
    const viewResult = viewColl.distinct(query);
    assert(arrayEq(collResult, viewResult), buildComparisonString(query, collResult, viewResult));
}
compareDistinctResults('x');
compareDistinctResults('nested.x');
compareDistinctResults('nested.nested.x');
compareDistinctResults('array');
compareDistinctResults('nested.array');
compareDistinctResults('documentArray');
compareDistinctResults('documentArray.x');
compareDistinctResults('documentArray[1].x');
compareDistinctResults('documentArray.1');
compareDistinctResults('documentArray.1.x');
compareDistinctResults('documentArray');
compareDistinctResults('doubleArray.x');
compareDistinctResults('doubleArray[1].x');
compareDistinctResults('doubleArray.1');
compareDistinctResults('doubleArray.1.x');
compareDistinctResults('0');
compareDistinctResults('0.x');
compareDistinctResults('1');
compareDistinctResults('1.0');
compareDistinctResults('2');
compareDistinctResults('2.0');
compareDistinctResults('numeric');
compareDistinctResults('arrayNumeric');

// Check that on binary 6.0 but FCV 5.3 a distinct on a view succeeds, though results may be
// different. Don't check results, as we know don't match the base collection distinct.
viewsDB.adminCommand({setFeatureCompatibilityVersion: "5.3"});
assert.commandWorked(
    viewsDB.runCommand({"distinct": viewColl.getName(), "key": "documentArray.1"}));
assert.commandWorked(viewsDB.runCommand({"distinct": viewColl.getName(), "key": "x"}));
assert.commandWorked(viewsDB.runCommand({"distinct": viewColl.getName(), "key": "0"}));
assert.commandWorked(viewsDB.runCommand({"distinct": viewColl.getName(), "key": "arrayNumeric"}));
// Reset FCV for future tests.
viewsDB.adminCommand({setFeatureCompatibilityVersion: "6.0"});

MongoRunner.stopMongod(conn);
})();
