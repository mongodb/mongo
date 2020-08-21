/**
 * Tests document validation behavior during an upgrade of a sharded cluster from 4.4 to 4.7+ and
 * during a corresponding downgrade as well as document validation behavior of mongod in FCV 4.4
 * mode.
 *
 * TODO SERVER-50524: this test is specific to the 4.4 - 4.7+ upgrade process, and can be removed
 * when 5.0 becomes last-lts.
 */
(function() {
"use strict";
load("jstests/multiVersion/libs/multi_cluster.js");  // For upgradeCluster.
load("jstests/libs/doc_validation_utils.js");        // For assertDocumentValidationFailure.
const collName = jsTestName();

/**
 * Performs a direct and an indirect (through aggregation stage $out) insertion of documents that
 * fail validation and checks the responses. 'sourceDB' is a database that is a source of documents
 * being copied into database 'targetDB' using aggregation stage $out. Both databases have a
 * collection named 'collName' and the collection in 'targetDB' has a validator set. 'assertFn' is a
 * function that verifies that the command result is a document validation error and conforms to
 * some FCV. The first parameter of the function is either a raw command response, or a wrapper of a
 * result of write commands ('BulkWriteResult' or 'WriteResult'), and the second - a collection
 * which documents are being inserted into.
 */
function testDocumentValidation(sourceDB, targetDB, assertFn) {
    const sourceColl = sourceDB[collName];

    // Insert a document into a collection in 'sourceDB'.
    assert.commandWorked(sourceColl.remove({}));
    assert.commandWorked(sourceColl.insert({a: 2}));

    // Issue an 'aggregate' command that copies all documents from the source collection to the
    // target collection.
    const res = sourceDB.runCommand(
        {aggregate: collName, pipeline: [{$out: {db: "targetDB", coll: collName}}], cursor: {}});

    // Verify that document insertion failed due to document validation error.
    assertFn(res, sourceColl);

    // Verify that a direct document insertion to a collection with a document validator fails due
    // to document validation error.
    assertFn(targetDB[collName].insert({a: 2}), targetDB[collName]);
}

/**
 * Assert that 'res' corresponds to DocumentValidationFailure, and verify that its format conforms
 * to FCV4.4 - field 'errInfo' is not present.
 */
function assertFCV44DocumentValidationFailure(res, coll) {
    assert.commandFailedWithCode(res, ErrorCodes.DocumentValidationFailure, tojson(res));
    if (coll.getMongo().writeMode() === "commands") {
        if (res instanceof BulkWriteResult) {
            const errors = res.getWriteErrors();
            for (const error of errors) {
                assert(!error.hasOwnProperty("errInfo"), tojson(error));
            }
        } else {
            const error = res instanceof WriteResult ? res.getWriteError() : res;
            assert(!error.hasOwnProperty("errInfo"), tojson(error));
        }
    }
}

// Test document validation behavior of mongod in FCV 4.4 mode.
(function() {
const mongod = MongoRunner.runMongod();
assert.neq(null, mongod, "mongod was unable to start up");
const testDB = mongod.getDB("test");

// Set FCV to 4.4.
assert.commandWorked(mongod.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

// Create a collection with a validator.
assert.commandWorked(testDB.createCollection(collName, {validator: {a: 1}}));

// Verify that a document insertion fails due to document validation error that conforms to FCV4.4.
assertFCV44DocumentValidationFailure(testDB[collName].insert({a: 2}), testDB[collName]);
MongoRunner.stopMongod(mongod);
})();

// Test document validation behavior during an upgrade of a sharded cluster from 4.4 to 4.7+ and a
// corresponding downgrade.
(function() {
// Start a sharded cluster in which all processes are of the 4.4 binVersion.
const st = new ShardingTest({
    shards: 2,
    rs: {nodes: 2, binVersion: "last-lts"},
    other: {mongosOptions: {binVersion: "last-lts"}, configOptions: {binVersion: "last-lts"}}
});
const mongos = st.s;

// Set cluster FCV to 4.4.
assert.commandWorked(mongos.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

// Two databases 'sourceDB' and 'targetDB' are setup that have different shards set as primary to
// test if document validation related aspects of inter-shard communication work correctly. This
// communication is triggered by issuing the aggregate command that reads documents from a
// collection in a database in one shard ('sourceDB') and inserts them into a database in another
// shard ('targetDB'). First create a database "sourceDB" and assign it to the first shard.
let sourceDB = mongos.getDB("sourceDB");
assert.commandWorked(sourceDB.createCollection(collName));
st.ensurePrimaryShard("sourceDB", st.shard0.shardName);

let targetDB = mongos.getDB("targetDB");

// Create a collection with a validator.
assert.commandWorked(targetDB.createCollection(collName, {validator: {a: 1}}));

// Assign database "targetDB" to the second shard.
st.ensurePrimaryShard("targetDB", st.shard1.shardName);

// Perform document insertion and verify output conformance to FCV4.4.
testDocumentValidation(sourceDB, targetDB, assertFCV44DocumentValidationFailure);

// Upgrade config servers and the second shard to latest version.
st.upgradeCluster("latest", {upgradeShards: false, upgradeConfigs: true, upgradeMongos: false});
st.rs1.upgradeSet({binVersion: "latest"});

// Perform document insertion and verify output conformance to FCV4.4.
testDocumentValidation(sourceDB, targetDB, assertFCV44DocumentValidationFailure);

// Upgrade the remaining shard.
st.upgradeCluster("latest", {upgradeShards: true, upgradeConfigs: false, upgradeMongos: false});

// Perform document insertion  and verify output conformance to FCV4.4.
testDocumentValidation(sourceDB, targetDB, assertFCV44DocumentValidationFailure);

// Upgrade the mongos.
st.upgradeCluster("latest", {upgradeShards: false, upgradeConfigs: false, upgradeMongos: true});
sourceDB = st.s.getDB("sourceDB");
targetDB = st.s.getDB("targetDB");

// Perform document insertion and verify output conformance to FCV4.4.
testDocumentValidation(sourceDB, targetDB, assertFCV44DocumentValidationFailure);

// Set FCV to 4.7.
assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

// Perform document insertion and verify that the server now provides the "errInfo" field, which
// contains the document validation failure details.
testDocumentValidation(sourceDB, targetDB, assertDocumentValidationFailure);

// Start a downgrade. Set FCV to 4.4.
assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

// Perform document insertion and verify output conformance to FCV4.4.
testDocumentValidation(sourceDB, targetDB, assertFCV44DocumentValidationFailure);

// Downgrade the mongos.
st.upgradeCluster("last-lts", {upgradeShards: false, upgradeConfigs: false, upgradeMongos: true});
sourceDB = st.s.getDB("sourceDB");
targetDB = st.s.getDB("targetDB");

// Perform document insertion and verify output conformance to FCV4.4.
testDocumentValidation(sourceDB, targetDB, assertFCV44DocumentValidationFailure);

// Downgrade the first shard.
st.rs0.upgradeSet({binVersion: "last-lts"});

// Perform document insertion and verify output conformance to FCV4.4.
testDocumentValidation(sourceDB, targetDB, assertFCV44DocumentValidationFailure);

// Downgrade remaining shards and the config servers.
st.upgradeCluster("last-lts", {upgradeShards: true, upgradeConfigs: true, upgradeMongos: false});

// Perform document insertion and verify output conformance to FCV4.4.
testDocumentValidation(sourceDB, targetDB, assertFCV44DocumentValidationFailure);
st.stop();
})();
})();