// @tags: [
//   # The test runs commands that are not allowed with security token: datasize, splitVector.
//   not_allowed_with_signed_security_token,
//   assumes_superuser_permissions,
//   # Cannot implicitly shard accessed collections because the "splitVector" command cannot be run
//   # on a sharded collection
//   assumes_unsharded_collection,
//   # rollbacks make WT RecordStore dataSize go out of sync, which negatively impacts the
//   # splitVector calculations.
//   does_not_support_stepdowns,
//   requires_collstats,
//   requires_fastcount,
//   no_selinux,
// ]

// -------------------------
//  SPLITVECTOR TEST UTILS
// -------------------------

// -------------------------
// assertChunkSizes verifies that a given 'splitVec' divides the 'test.jstest_splitvector'
// collection in 'maxChunkSize' approximately-sized chunks. Its asserts fail otherwise.
// @param splitVec: an array with keys for field 'x'
//        e.g. [ { x : 1927 }, { x : 3855 }, ...
// @param numDocs: domain of 'x' field
//        e.g. 20000
// @param maxChunkSize is in MBs.
//
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

let assertChunkSizes = function (splitVec, numDocs, maxChunkSize, msg) {
    splitVec = [{x: -1}].concat(splitVec);
    splitVec.push({x: numDocs + 1});
    for (let i = 0; i < splitVec.length - 1; i++) {
        let min = splitVec[i];
        let max = splitVec[i + 1];
        let avgObjSize = db.jstests_splitvector.stats().avgObjSize;
        let size = db.runCommand({datasize: "test.jstests_splitvector", min: min, max: max}).size;

        // It is okay for the last chunk to be  smaller. A collection's size does not
        // need to be exactly a multiple of maxChunkSize.
        if (i < splitVec.length - 2) {
            // We are within one object of the correct chunk size.
            assert.lt(Math.abs(maxChunkSize - size), avgObjSize, "Assertion failed", {msg: msg + "b", chunk: "A" + i});
        } else {
            assert.gt(maxChunkSize, size, "Assertion failed", {msg: msg + "b", chunk: "A" + i});
        }
    }
};

// Takes two documents and asserts that both contain exactly the same set of field names.
// This is useful for checking that splitPoints have the same format as the original key pattern,
// even when sharding on a prefix key.
// Not very efficient, so only call when # of field names is small
let assertFieldNamesMatch = function (splitPoint, keyPattern) {
    for (var p in splitPoint) {
        if (splitPoint.hasOwnProperty(p)) {
            assert(keyPattern.hasOwnProperty(p), "property " + p + " not in keyPattern");
        }
    }
    for (var p in keyPattern) {
        if (keyPattern.hasOwnProperty(p)) {
            assert(splitPoint.hasOwnProperty(p), "property " + p + " not in splitPoint");
        }
    }
};

let resetCollection = function () {
    f.drop();
};

// Inserts numDocs into the given collection using a bulk operation. Each document's x value is its
// index within the batch, starting from 1, and the y value is the given filler.
let bulkInsertDocs = function (coll, numDocs, filler) {
    const bulk = coll.initializeUnorderedBulkOp();
    for (let i = 1; i <= numDocs; i++) {
        bulk.insert({x: i, y: filler});
    }
    assert.commandWorked(bulk.execute());
};

// Inserts numDocs into the given collection using a bulk operation. Each document's x value is set
// to the given xVal and the y value is the given filler.
let bulkInsertDocsFixedX = function (coll, numDocs, filler, xVal) {
    const bulk = coll.initializeUnorderedBulkOp();
    for (let i = 1; i <= numDocs; i++) {
        bulk.insert({x: xVal, y: filler});
    }
    assert.commandWorked(bulk.execute());
};

// -------------------------
//  TESTS START HERE
// -------------------------
let f = db.jstests_splitvector;
resetCollection();

// -------------------------
// Case 1: missing parameters

assert.eq(false, db.runCommand({splitVector: "test.jstests_splitvector"}).ok, "1a");
assert.eq(false, db.runCommand({splitVector: "test.jstests_splitvector", maxChunkSize: 1}).ok, "1b");

// -------------------------
// Case 2: missing index

assert.eq(false, db.runCommand({splitVector: "test.jstests_splitvector", keyPattern: {x: 1}, maxChunkSize: 1}).ok, "2");

// -------------------------
// Case 3: empty collection
f.createIndex({x: 1});
assert.eq(
    [],
    db.runCommand({splitVector: "test.jstests_splitvector", keyPattern: {x: 1}, maxChunkSize: 1}).splitKeys,
    "3",
);

// -------------------------
// Case 4: uniform collection

resetCollection();
f.createIndex({x: 1});

let filler;
let case4 = function () {
    // Get baseline document size
    filler = "";
    while (filler.length < 500) filler += "a";
    f.save({x: 0, y: filler});
    let docSize = db.runCommand({datasize: "test.jstests_splitvector"}).size;
    assert.gt(docSize, 500, "4a");

    // Fill collection and get split vector for 1MB maxChunkSize
    let numDocs = 4500;
    bulkInsertDocs(f, numDocs - 1, filler); // 1 document was already inserted.
    let res = db.runCommand({splitVector: "test.jstests_splitvector", keyPattern: {x: 1}, maxChunkSize: 1});

    // splitVector aims at getting half-full chunks after split
    let factor = 0.5;

    assert.eq(true, res.ok, "4b");

    assert.close((numDocs * docSize) / ((1 << 20) * factor), res.splitKeys.length, "num split keys", -1);
    assertChunkSizes(res.splitKeys, numDocs, (1 << 20) * factor, "4d");
    for (let i = 0; i < res.splitKeys.length; i++) {
        assertFieldNamesMatch(res.splitKeys[i], {x: 1});
    }
};
case4();

// -------------------------
// Case 5: limit number of split points

resetCollection();
f.createIndex({x: 1});

let case5 = function () {
    // Fill collection and get split vector for 1MB maxChunkSize
    bulkInsertDocs(f, 4499, filler);
    let res = db.runCommand({
        splitVector: "test.jstests_splitvector",
        keyPattern: {x: 1},
        maxChunkSize: 1,
        maxSplitPoints: 1,
    });

    assert.eq(true, res.ok, "5a");
    assert.eq(1, res.splitKeys.length, "5b");
    for (let i = 0; i < res.splitKeys.length; i++) {
        assertFieldNamesMatch(res.splitKeys[i], {x: 1});
    }
};
case5();

// -------------------------
// Case 6: limit number of objects in a chunk

resetCollection();
f.createIndex({x: 1});

let case6 = function () {
    // Fill collection and get split vector for 1MB maxChunkSize
    bulkInsertDocs(f, 1999, filler);
    let res = db.runCommand({
        splitVector: "test.jstests_splitvector",
        keyPattern: {x: 1},
        maxChunkSize: 1,
        maxChunkObjects: 500,
    });

    assert.eq(true, res.ok, "6a");
    assert.eq(3, res.splitKeys.length, "6b");
    for (let i = 0; i < res.splitKeys.length; i++) {
        assertFieldNamesMatch(res.splitKeys[i], {x: 1});
    }
};
case6();

// -------------------------
// Case 7: enough occurances of min key documents to pass the chunk limit
// [1111111111111111,2,3)

resetCollection();
f.createIndex({x: 1});

let case7 = function () {
    // Fill collection and get split vector for 1MB maxChunkSize
    bulkInsertDocsFixedX(f, 2099, filler, 1);
    bulkInsertDocsFixedX(f, 9, filler, 2);
    let res = db.runCommand({splitVector: "test.jstests_splitvector", keyPattern: {x: 1}, maxChunkSize: 1});

    assert.eq(true, res.ok, "7a");
    assert.eq(2, res.splitKeys[0].x, "7b");
    for (let i = 0; i < res.splitKeys.length; i++) {
        assertFieldNamesMatch(res.splitKeys[i], {x: 1});
    }
};
case7();

// -------------------------
// Case 8: few occurrances of min key, and enough of some other that we cannot split it
// [1, 22222222222222, 3)

resetCollection();
f.createIndex({x: 1});

let case8 = function () {
    bulkInsertDocsFixedX(f, 9, filler, 1);
    bulkInsertDocsFixedX(f, 2099, filler, 2);
    bulkInsertDocsFixedX(f, 9, filler, 3);
    let res = db.runCommand({splitVector: "test.jstests_splitvector", keyPattern: {x: 1}, maxChunkSize: 1});

    assert.eq(true, res.ok, "8a");
    assert.eq(2, res.splitKeys.length, "8b");
    assert.eq(2, res.splitKeys[0].x, "8c");
    assert.eq(3, res.splitKeys[1].x, "8d");
    for (let i = 0; i < res.splitKeys.length; i++) {
        assertFieldNamesMatch(res.splitKeys[i], {x: 1});
    }
};
case8();

// -------------------------
// Case 9: splitVector "force" mode, where we split (possible small) chunks in the middle
//

resetCollection();
f.createIndex({x: 1});

let case9 = function () {
    f.save({x: 1});
    f.save({x: 2});
    f.save({x: 3});

    assert.eq(3, f.count());
    print(f.getFullName());

    let res = db.runCommand({splitVector: f.getFullName(), keyPattern: {x: 1}, force: true});

    assert.eq(true, res.ok, "9a");
    assert.eq(1, res.splitKeys.length, "9b");
    assert.eq(2, res.splitKeys[0].x, "9c");

    // TODO SERVER-87574 remove !TestData.testingReplicaSetEndpoint
    if (db.runCommand("hello").msg != "isdbgrid" && !TestData.testingReplicaSetEndpoint) {
        res = db.adminCommand({splitVector: "test.jstests_splitvector", keyPattern: {x: 1}, force: true});

        assert.eq(true, res.ok, "9a: " + tojson(res));
        assert.eq(1, res.splitKeys.length, "9b: " + tojson(res));
        assert.eq(2, res.splitKeys[0].x, "9c: " + tojson(res));
        for (let i = 0; i < res.splitKeys.length; i++) {
            assertFieldNamesMatch(res.splitKeys[i], {x: 1});
        }
    }
};
case9();

// -------------------------
// Repeat all cases using prefix shard key.
//

resetCollection();
f.createIndex({x: 1, y: 1});
case4();

resetCollection();
f.createIndex({x: 1, y: -1, z: 1});
case4();

resetCollection();
f.createIndex({x: 1, y: 1});
case5();

resetCollection();
f.createIndex({x: 1, y: -1, z: 1});
case5();

resetCollection();
f.createIndex({x: 1, y: 1});
case6();

resetCollection();
f.createIndex({x: 1, y: -1, z: 1});
case6();

resetCollection();
f.createIndex({x: 1, y: 1});
case7();

resetCollection();
f.createIndex({x: 1, y: -1, z: 1});
case7();

resetCollection();
f.createIndex({x: 1, y: 1});
case8();

resetCollection();
f.createIndex({x: 1, y: -1, z: 1});
case8();

resetCollection();
f.createIndex({x: 1, y: 1});
case9();

resetCollection();
f.createIndex({x: 1, y: -1, z: 1});
case9();

print("PASSED");
