/**
 * Tests that the server will startup normally when a collection has a missing id index.
 *
 * @tags: [requires_persistence, requires_replication]
 */
import {IndexCatalogHelpers} from "jstests/libs/index_catalog_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

let replSet = new ReplSetTest({nodes: 1});
replSet.startSet();
replSet.initiate();

let conn = replSet.getPrimary();

const dbName = jsTestName();
const collName = "coll";

let testDB = conn.getDB(dbName);
let coll = testDB.getCollection(collName);

assert.commandWorked(testDB.adminCommand({configureFailPoint: "skipIdIndex", mode: "alwaysOn"}));
assert.commandWorked(testDB.createCollection(collName));

assert.commandWorked(coll.insert({_id: 0, a: 1}));

let indexSpec = IndexCatalogHelpers.findByName(coll.getIndexes(), "_id_");
assert.eq(indexSpec, null);

let foundException = false;
try {
    replSet.restart(conn);
} catch (e) {
    foundException = RegExp("9271000.*").test(rawMongoProgramOutput());
}
assert(foundException);

replSet.stopSet();