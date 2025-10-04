/**
 * Initiates a background index build on the primary, and while the secondary is building the index
 * through replication, the primary drops all the indexes.
 * @tags: [
 *   requires_replication,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

let dbname = jsTestName();
let collection = "jstests_feh";
let size = 100;

const replTest = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
                votes: 0,
            },
            slowms: 30000, // Don't log slow operations on secondary. See SERVER-44821.
        },
    ],
});
const nodes = replTest.startSet();
replTest.initiate();

let primary = replTest.getPrimary();
let second = replTest.getSecondary();

let primaryDB = primary.getDB(dbname);
let secondDB = second.getDB(dbname);

let dc = {dropIndexes: collection, index: "i_1"};

// Setup collections.
primaryDB.dropDatabase();
jsTest.log("Creating test data " + size + " documents");
Random.setRandomSeed();
let bulk = primaryDB.getCollection(collection).initializeUnorderedBulkOp();
for (i = 0; i < size; ++i) {
    bulk.insert({i: Random.rand()});
}
assert.commandWorked(bulk.execute({w: 2, wtimeout: replTest.timeoutMS}));

assert.commandWorked(secondDB.adminCommand({configureFailPoint: "hangAfterStartingIndexBuild", mode: "alwaysOn"}));

jsTest.log("Starting background indexing for test of: " + tojson(dc));

// Add another index to be sure the drop command works.
primaryDB.getCollection(collection).createIndex({b: 1});
primaryDB.getCollection(collection).createIndex({i: 1});

// Make sure the index build has started on the secondary.
IndexBuildTest.waitForIndexBuildToStart(secondDB);

jsTest.log("Dropping indexes");
primaryDB.runCommand({dropIndexes: collection, index: "*"});

jsTest.log("Waiting on replication");
assert.commandWorked(secondDB.adminCommand({configureFailPoint: "hangAfterStartingIndexBuild", mode: "off"}));
replTest.awaitReplication();

print("Index list on primary:");
primaryDB.getCollection(collection).getIndexes().forEach(printjson);

// Need to assert.soon because the drop only marks the index for removal
// the removal itself is asynchronous and may take another moment before it happens.
var i = 0;
assert.soon(function () {
    print("Index list on secondary (run " + i + "):");
    secondDB.getCollection(collection).getIndexes().forEach(printjson);

    i++;
    return 1 === secondDB.getCollection(collection).getIndexes().length;
}, "secondary did not drop index");

replTest.stopSet();
