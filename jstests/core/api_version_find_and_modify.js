/**
 * Tests the findAndModify command under different scenarios with API versioning enabled.
 *
 * @tags: [
 *     uses_api_parameters,
 *     requires_fcv_49,
 *     assumes_unsharded_collection,
 * ]
 */
(function() {
"use strict";

const testDB = db.getSiblingDB("findAndModifyAPIVersion");
testDB.dropDatabase();

const collName = "testColl";
const coll = testDB.getCollection(collName);
const incStockFactor = 10;
let curStock = 10;

assert.commandWorked(coll.insert({itemNumber: 1, stockUnit: curStock}));

const assertStockUnits = function() {
    const result = coll.find({itemNumber: 1}).toArray();
    assert.eq(result.length, 1);
    curStock += incStockFactor;
    assert.eq(result[0].stockUnit, curStock);
};

// Test the command with latest command name and 'apiStrict'.
assert.commandWorked(testDB.runCommand({
    findAndModify: collName,
    query: {itemNumber: 1},
    update: {"$inc": {stockUnit: incStockFactor}},
    apiVersion: "1",
    apiStrict: true
}));
assertStockUnits();

// Test the command with command alias 'findandmodify' and 'apiStrict'.
const result = testDB.runCommand({
    findandmodify: collName,
    query: {itemNumber: 1},
    update: {"$inc": {stockUnit: incStockFactor}},
    apiVersion: "1",
    apiStrict: true
});
assert.commandFailedWithCode(result, ErrorCodes.APIStrictError);

// Test the command with latest command name without 'apiStrict'.
assert.commandWorked(testDB.runCommand({
    findAndModify: collName,
    query: {itemNumber: 1},
    update: {"$inc": {stockUnit: incStockFactor}},
    apiVersion: "1"
}));
assertStockUnits();

// Test the command with command alias 'findandmodify' without 'apiStrict'.
assert.commandWorked(testDB.runCommand({
    findandmodify: collName,
    query: {itemNumber: 1},
    update: {"$inc": {stockUnit: incStockFactor}},
    apiVersion: "1"
}));
assertStockUnits();
})();
