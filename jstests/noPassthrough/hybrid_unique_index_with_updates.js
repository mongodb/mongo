/**
 * Tests that write operations are accepted and result in correct indexing behavior for each phase
 * of hybrid unique index builds. This test inserts a duplicate document at different phases of an
 * index build to confirm that the resulting behavior is failure.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

let replSetTest = new ReplSetTest({name: "hybrid_updates", nodes: 2});
replSetTest.startSet();
replSetTest.initiate();

let conn = replSetTest.getPrimary();
let testDB = conn.getDB('test');

// Enables a failpoint, runs 'hitFailpointFunc' to hit the failpoint, then runs
// 'duringFailpointFunc' while the failpoint is active.
let doDuringFailpoint = function(
    failPointName, structuredLogRegEx, hitFailpointFunc, duringFailpointFunc, stopKey) {
    clearRawMongoProgramOutput();
    assert.commandWorked(testDB.adminCommand({
        configureFailPoint: failPointName,
        mode: "alwaysOn",
        data: {fieldsToMatch: {i: stopKey}}
    }));

    hitFailpointFunc();

    assert.soon(() => structuredLogRegEx.test(rawMongoProgramOutput()));

    duringFailpointFunc();

    assert.commandWorked(testDB.adminCommand({configureFailPoint: failPointName, mode: "off"}));
};

const docsToInsert = 1000;
let setUp = function(coll) {
    coll.drop();

    let bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < docsToInsert; i++) {
        bulk.insert({i: i});
    }
    assert.commandWorked(bulk.execute());
};

let buildIndexInBackground = function(coll, expectDuplicateKeyError) {
    const createIndexFunction = function(collFullName) {
        const coll = db.getMongo().getCollection(collFullName);
        return coll.createIndex({i: 1}, {background: true, unique: true});
    };
    const assertFunction = expectDuplicateKeyError ? function(collFullName) {
        assert.commandFailedWithCode(createIndexFunction(collFullName), ErrorCodes.DuplicateKey);
    } : function(collFullName) {
        assert.commandWorked(createIndexFunction(collFullName));
    };
    return startParallelShell('const createIndexFunction = ' + createIndexFunction + ';\n' +
                                  'const assertFunction = ' + assertFunction + ';\n' +
                                  'assertFunction("' + coll.getFullName() + '")',
                              conn.port);
};

/**
 * Run a background index build on a unique index under different configurations. Introduce
 * duplicate keys on the index that may cause it to fail or succeed, depending on the following
 * optional parameters:
 * {
 *   // Which operation used to introduce a duplicate key.
 *   operation {string}: "insert", "update"
 *
 *   // Whether or not resolve the duplicate key before completing the build.
 *   resolve {bool}
 *
 *   // Which phase of the index build to introduce the duplicate key.
 *   phase {number}: 0-4
 * }
 */
let runTest = function(config) {
    jsTestLog("running test with config: " + tojson(config));

    const collName = Object.keys(config).length
        ? 'hybrid_' + config.operation[0] + '_r' + Number(config.resolve) + '_p' + config.phase
        : 'hybrid';
    const coll = testDB.getCollection(collName);
    setUp(coll);

    // Expect the build to fail with a duplicate key error if we insert a duplicate key and
    // don't resolve it.
    let expectDuplicate = config.resolve === false;

    let awaitBuild;
    let buildIndex = function() {
        awaitBuild = buildIndexInBackground(coll, expectDuplicate);
    };

    // Introduce a duplicate key, either from an insert or update. Optionally, follow-up with an
    // operation that will resolve the duplicate by removing it or updating it.
    const dup = {i: 0};
    let doOperation = function() {
        if ("insert" == config.operation) {
            assert.commandWorked(coll.insert(dup));
            if (config.resolve) {
                assert.commandWorked(coll.deleteOne(dup));
            }
        } else if ("update" == config.operation) {
            assert.commandWorked(coll.update(dup, {i: 1}));
            if (config.resolve) {
                assert.commandWorked(coll.update({i: 1}, dup));
            }
        }
    };

    const stopKey = 0;
    switch (config.phase) {
        // Just build the index without any failpoints.
        case undefined:
            buildIndex();
            break;
        // Hang before scanning the first document.
        case 0:
            doDuringFailpoint(
                "hangIndexBuildDuringCollectionScanPhaseBeforeInsertion",
                new RegExp("\"id\":20386.*\"where\":\"before\",\"doc\":.*\"i\":" + stopKey),
                buildIndex,
                doOperation,
                stopKey);
            break;
        // Hang after scanning the first document.
        case 1:
            doDuringFailpoint(
                "hangIndexBuildDuringCollectionScanPhaseAfterInsertion",
                new RegExp("\"id\":20386.*\"where\":\"after\",\"doc\":.*\"i\":" + stopKey),
                buildIndex,
                doOperation,
                stopKey);
            break;
        // Hang before the first drain and after dumping the keys from the external sorter into
        // the index.
        case 2:
            doDuringFailpoint("hangAfterIndexBuildDumpsInsertsFromBulk",
                              new RegExp("\"id\":20665"),
                              buildIndex,
                              doOperation);
            break;
        // Hang before the second drain.
        case 3:
            doDuringFailpoint("hangAfterIndexBuildFirstDrain",
                              new RegExp("\"id\":20666"),
                              buildIndex,
                              doOperation);
            break;
        // Hang before the final drain and commit.
        case 4:
            doDuringFailpoint("hangAfterIndexBuildSecondDrain",
                              new RegExp("\"id\":20667"),
                              buildIndex,
                              doOperation);
            break;
        default:
            assert(false, "Invalid phase: " + config.phase);
    }

    awaitBuild();

    let expectedDocs = docsToInsert;
    expectedDocs += (config.operation == "insert" && config.resolve === false) ? 1 : 0;

    assert.eq(expectedDocs, coll.count());
    assert.eq(expectedDocs, coll.find().itcount());
    assert.commandWorked(coll.validate({full: true}));
};

runTest({});

for (let i = 0; i <= 4; i++) {
    runTest({operation: "insert", resolve: true, phase: i});
    runTest({operation: "insert", resolve: false, phase: i});
    runTest({operation: "update", resolve: true, phase: i});
    runTest({operation: "update", resolve: false, phase: i});
}

replSetTest.stopSet();
})();
