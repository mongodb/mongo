/**
 * Verifying that setting atClusterTime on extended validate performs timestamped reads.
 *
 * @tags: [requires_wiredtiger, requires_persistence]
 *
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Skip DB hash check in stopSet() since we expect it to fail in this test along with fastcount.
TestData.skipCheckDBHashes = true;
TestData.skipEnforceFastCountOnValidate = true;

const rst = new ReplSetTest({nodes: 3, settings: {chainingAllowed: false}});
rst.startSet();
rst.initiate();
let primary = rst.getPrimary();
let secondary = rst.getSecondary();

let db = primary.getDB("test");

assert(db.coll.drop());
assert.commandWorked(db.createCollection("coll"));

let res1 = assert.commandWorked(db.runCommand({insert: "coll", "documents": [{_id: 1}]}));
res1 = assert.commandWorked(db.runCommand({insert: "coll", "documents": [{_id: 2}]}));

// Save the opTime we want to validate at
let opTime = res1.operationTime;

// Make sure the writes make it to all nodes
rst.awaitLastOpCommitted();

jsTest.log.info("Validate with background: true and atClusterTime should fail");
assert.commandFailed(db.runCommand({validate: "coll", background: true, collHash: true, atClusterTime: opTime}));

jsTest.log.info("Validate with atClusterTime on an unreplicated collection should fail");
assert.commandFailed(
    primary.getDB("local").runCommand({validate: "oplog.rs", background: true, collHash: true, atClusterTime: opTime}),
);

const fp = configureFailPoint(secondary, "stopReplProducer");

// Insert a write on the primary that does not replicate to the secondary.
res1 = assert.commandWorked(db.runCommand({insert: "coll", "documents": [{_id: 3}]}));

// Validate with no readConcern should not be equal since extra document on the secondary
jsTest.log.info("Validate with no readConcern should not be equal since extra document on the secondary");
res1 = assert.commandWorked(db.runCommand({validate: "coll", background: false, collHash: true}));
let res2 = assert.commandWorked(
    secondary.getDB("test").runCommand({validate: "coll", background: false, collHash: true}),
);

assert.eq(false, res1.all == res2.all, `res1: ${tojson(res1)}, res2: ${tojson(res2)}`);

// Validate with readConcern should be equal
jsTest.log.info("Validate with readConcern should be equal");
res1 = assert.commandWorked(
    db.runCommand({validate: "coll", background: false, collHash: true, atClusterTime: opTime}),
);
res2 = assert.commandWorked(
    secondary.getDB("test").runCommand({validate: "coll", background: false, collHash: true, atClusterTime: opTime}),
);

assert.eq(true, res1.all == res2.all, `res1: ${tojson(res1)}, res2: ${tojson(res2)}`);

let primaryPath = primary.dbpath;
let secondaryPath = secondary.dbpath;

rst.stopSet(null /* signal */, true /* forRestart */);

function runValidate(path, opts) {
    MongoRunner.runMongod({
        dbpath: path,
        validate: "",
        setParameter: {
            validateDbName: "test",
            validateCollectionName: "coll",
            collectionValidateOptions: {options: opts},
        },
        noCleanData: true,
    });
    const validateResults = rawMongoProgramOutput("(9437301)")
        .split("\n")
        .filter((line) => line.trim() !== "")
        .map((line) => JSON.parse(line.split("|").slice(1).join("|")));
    assert.eq(validateResults.length, 1);
    // jsTest.log.info(`Validate result \n${tojson(validateResults[0])}`);
    clearRawMongoProgramOutput();
    return validateResults[0].attr.results;
}

jsTest.log.info("Modal Validate Tests");

// Validate with no readConcern should not be equal due to extra document on the secondary
jsTest.log.info("Validate with no readConcern should not be equal due to extra document on the secondary");
res1 = runValidate(primaryPath, {collHash: true});
res2 = runValidate(secondaryPath, {collHash: true});

assert.eq(false, res1.all == res2.all, `res1: ${tojson(res1)}, res2: ${tojson(res2)}`);

// Validate drill down should be different due to extra document on the secondary
jsTest.log.info("Validate drill down should be different due to extra document on the secondary");
res1 = runValidate(primaryPath, {collHash: true, hashPrefixes: []});
res2 = runValidate(secondaryPath, {collHash: true, hashPrefixes: []});

assert.eq(true, res1.all == res2.all, `res1: ${tojson(res1)}, res2: ${tojson(res2)}`);

let keys1 = Object.keys(res1.partial);
let keys2 = Object.keys(res2.partial);

let inconsistency = keys1.length === keys2.length;
for (const key of keys1) {
    inconsistency = inconsistency || !keys2.includes(key) || res2.partial[key].hash !== res1.partial[key].hash;
}
assert(inconsistency, `res1: ${tojson(res1)}, res2: ${tojson(res2)}`);

// Validate should be equal at cluster time
jsTest.log.info("Validate should be equal at cluster time");
res1 = runValidate(primaryPath, {collHash: true, atClusterTime: opTime});
res2 = runValidate(secondaryPath, {collHash: true, atClusterTime: opTime});

assert.eq(true, res1.all == res2.all, `res1: ${tojson(res1)}, res2: ${tojson(res2)}`);

// Validate drill down should be equal at cluster time
jsTest.log.info("Validate drill down should be equal at cluster time");
res1 = runValidate(primaryPath, {collHash: true, hashPrefixes: [], atClusterTime: opTime});
res2 = runValidate(secondaryPath, {collHash: true, hashPrefixes: [], atClusterTime: opTime});

assert.eq(true, res1.all == res2.all, `res1: ${tojson(res1)}, res2: ${tojson(res2)}`);

keys1 = Object.keys(res1.partial);
keys2 = Object.keys(res2.partial);

assert.eq(keys1.length, keys2.length, `res1: ${tojson(res1)}, res2: ${tojson(res2)}`);
for (const key of keys1) {
    assert(
        keys2.includes(key) && res2.partial[key].hash === res1.partial[key].hash,
        `res1: ${tojson(res1)}, res2: ${tojson(res2)}`,
    );
}
