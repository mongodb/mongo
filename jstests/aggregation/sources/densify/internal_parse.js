/**
 * Test the syntax of $_internalDensify.
 * @tags: [
 *     # Needed as $densify is a 51 feature.
 *     requires_fcv_51,
 *     # Our method of connecting via an internal client requries an unsharded topology.
 *     assumes_unsharded_collection,
 *     assumes_against_mongod_not_mongos,
 *     assumes_read_preference_unchanged,
 * ]
 */

(function() {
"use strict";

load("jstests/aggregation/sources/densify/libs/parse_util.js");

const dbName = jsTestName();
const testDB = db.getSiblingDB(dbName);
testDB.dropDatabase();
const collName = jsTestName();

const testInternalClient = (function createInternalClient() {
    const connInternal = new Mongo(testDB.getMongo().host);
    const curDB = connInternal.getDB(dbName);
    assert.commandWorked(curDB.runCommand({
        ["hello"]: 1,
        internalClient: {minWireVersion: NumberInt(0), maxWireVersion: NumberInt(7)}
    }));
    return connInternal;
})();

const internalDB = testInternalClient.getDB(dbName);
const internalColl = internalDB[collName];

parseUtil(internalDB, internalColl, "$_internalDensify", {
    writeConcern: {w: "majority"},
});
})();
