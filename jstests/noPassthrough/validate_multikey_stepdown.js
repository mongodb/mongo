/**
 * Validates multikey index in a collection after retrying a 2dsphere insert.
 * The scenario tested here involves stepping down the node so that the first insert attempt fails.
 * The insert is retried after the node becomes primary again. At this point, we will validate the
 * collection after restarting the node.
 * @tags: [
 *     requires_replication,
 *     requires_persistence,
 * ]
 */
(function() {
'use strict';

load('jstests/libs/fail_point_util.js');
load('jstests/libs/parallel_shell_helpers.js');

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
let testColl = primary.getCollection('test.validate_multikey_stepdown');

assert.commandWorked(testColl.getDB().createCollection(testColl.getName()));

assert.commandWorked(testColl.createIndex({geo: '2dsphere'}));

// Sample polygon and point from geo_s2dedupnear.js
const polygon = {
    type: 'Polygon',
    coordinates: [[
        [100.0, 0.0],
        [101.0, 0.0],
        [101.0, 1.0],
        [100.0, 1.0],
        [100.0, 0.0],
    ]],
};
const point = {
    type: 'Point',
    coordinates: [31, 41],
};
const geoDocToInsert = {
    _id: 1,
    geo: polygon,
};
const geoQuery = {
    geo: {$geoNear: point}
};

const failPoint = configureFailPoint(
    primary, 'hangAfterCollectionInserts', {collectionNS: testColl.getFullName()});

let awaitInsert;
try {
    const args = [testColl.getDB().getName(), testColl.getName(), geoDocToInsert];
    let func = function(args) {
        const [dbName, collName, geoDocToInsert] = args;
        jsTestLog("Insert a document that will hang before the insert completes.");
        const testColl = db.getSiblingDB(dbName)[collName];
        // This should fail with ErrorCodes.InterruptedDueToReplStateChange.
        const result = testColl.insert(geoDocToInsert);
        jsTestLog('Async insert result = ' + tojson(result));
        assert.commandFailedWithCode(result, ErrorCodes.InterruptedDueToReplStateChange);
    };
    awaitInsert = startParallelShell(funWithArgs(func, args), primary.port);

    jsTest.log("Wait for async insert to hit the failpoint.");
    failPoint.wait();

    // Step down the primary. This will interrupt the async insert.
    // Since there is no other electable node, the replSetStepDown command will time out and the
    // node will be re-elected.
    jsTest.log("Step down primary temporarily to interrupt async insert.");
    assert.commandFailedWithCode(primary.adminCommand({replSetStepDown: 10}),
                                 ErrorCodes.ExceededTimeLimit);
} finally {
    // Turn off the failpoint before allowing the test to end, so nothing hangs while the server
    // shuts down or in post-test hooks.
    failPoint.off();
}

// Wait until the async insert is completed.
jsTest.log("Wait for async insert to complete.");
awaitInsert();

jsTest.log("Retrying insert after stepping up.");
assert.commandWorked(testColl.insert(geoDocToInsert));

jsTestLog('Checking documents in collection before restart');
let docs = testColl.find(geoQuery).sort({_id: 1}).toArray();
assert.eq(1, docs.length, 'too many docs in collection: ' + tojson(docs));
assert.eq(
    geoDocToInsert._id, docs[0]._id, 'unexpected document content in collection: ' + tojson(docs));

// For the purpose of reproducing the validation error in geo_2dsphere, it is important to skip
// validation when restarting the primary node. Enabling validation here has an effect on the
// validate command's behavior after restarting.
primary = rst.restart(primary, {skipValidation: true}, /*signal=*/undefined, /*wait=*/true);
testColl = primary.getCollection(testColl.getFullName());

jsTestLog('Checking documents in collection after restart');
rst.awaitReplication();
docs = testColl.find(geoQuery).sort({_id: 1}).toArray();
assert.eq(1, docs.length, 'too many docs in collection: ' + tojson(docs));
assert.eq(
    geoDocToInsert._id, docs[0]._id, 'unexpected document content in collection: ' + tojson(docs));

jsTestLog('Validating collection after restart');
const result = assert.commandWorked(testColl.validate({full: true}));

jsTestLog('Validation result: ' + tojson(result));
assert.eq(testColl.getFullName(), result.ns, tojson(result));
assert.eq(0, result.nInvalidDocuments, tojson(result));
assert.eq(1, result.nrecords, tojson(result));
assert.eq(2, result.nIndexes, tojson(result));

// Check non-geo indexes.
assert.eq(1, result.keysPerIndex._id_, tojson(result));
assert(result.indexDetails._id_.valid, tojson(result));

// Check geo index.
assert.lt(1, result.keysPerIndex.geo_2dsphere, tojson(result));
assert(result.indexDetails.geo_2dsphere.valid, tojson(result));

assert(result.valid, tojson(result));

rst.stopSet();
})();
