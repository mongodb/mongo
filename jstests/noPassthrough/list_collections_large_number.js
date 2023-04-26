/**
 * Tests that "listCollections" command successfully returns results when the database has a very
 * large number of collections.
 * @tags: [
 *  resource_intensive,
 * ]
 */
(function() {
"use strict";

const conn = MongoRunner.runMongod({});
assert.neq(null, conn, "mongod was unable to start up");
const db = conn.getDB(jsTestName());

const validatorObj = {
    $jsonSchema: {
        bsonType: "object",
        properties: {
            s: {bsonType: "string", description: "x".repeat(4801)},

        }
    }
};
const nCollections = 3300;
jsTestLog(`Creating ${nCollections} collections....`);
for (let i = 0; i < nCollections; i++) {
    assert.commandWorked(db.createCollection("c_" + i.toPrecision(6), {validator: validatorObj}));
}
jsTestLog(`Done creating ${nCollections} collections`);
assert.commandWorked(db.runCommand({"listCollections": 1}));

// Do not validate collections since that is an expensive action.
MongoRunner.stopMongod(conn, undefined, {skipValidation: true});
})();
