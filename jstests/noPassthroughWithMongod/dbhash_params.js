/**
 * Tests that dbhash accepts collection, minKey, and maxKey params correctly.
 * This test validates the fix for SERVER-74681.
 *
 * @tags: [
 *   # dbhash command is not available on embedded
 *   incompatible_with_embedded,
 *   # "minKey" and "maxKey" params are not supported until bin version 7.0
 *   requires_fcv_70,
 * ]
 */

(function() {
"use strict";

const NULL_MD5 = "d41d8cd98f00b204e9800998ecf8427e";

function testDbHash(collName, collOptions) {
    const coll = collName;
    const copyColl = collName + "_copy";
    const subColl = collName + "_sub";

    db[coll].drop();
    db[copyColl].drop();
    db[subColl].drop();

    assert.commandWorked(db.createCollection(coll, collOptions));
    assert.commandWorked(db.createCollection(copyColl, collOptions));
    assert.commandWorked(db.createCollection(subColl, collOptions));

    for (let i = 0; i < 10; i++) {
        const docI = {_id: i, a: i};
        assert.commandWorked(db[coll].insert(docI));
        assert.commandWorked(db[copyColl].insert(docI));

        // Only put docs 3,4,5,6 in `subColl` so it doesn't match `coll` and `copyColl`.
        if (i > 2 && i < 7) {
            assert.commandWorked(db[subColl].insert(docI));
        }
    }

    jsTestLog("No params, hash of entire db");
    let hash = assert.commandWorked(db.runCommand({dbhash: 1}));
    assert.eq(hash.collections[coll], hash.collections[copyColl], hash);
    assert.neq(hash.collections[coll], hash.collections[subColl], hash);

    jsTestLog("Collection option, no minKey or maxKey");
    hash = assert.commandWorked(db.runCommand({dbhash: 1, collections: [coll, subColl]}));
    assert(!hash.collections.hasOwnProperty(copyColl), hash);
    assert.neq(hash.collections[coll], hash.collections[subColl], hash);

    jsTestLog("minKey and maxKey with no collection param errors");
    assert.commandFailedWithCode(db.runCommand({dbhash: 1, minKey: 1}), ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(db.runCommand({dbhash: 1, maxKey: 1}), ErrorCodes.InvalidOptions);

    jsTestLog("minKey and maxKey with multiple collections errors");
    assert.commandFailedWithCode(
        db.runCommand({dbhash: 1, collections: [coll, subColl], minKey: 1}),
        ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(
        db.runCommand({dbhash: 1, collections: [coll, subColl], maxKey: 1}),
        ErrorCodes.InvalidOptions);

    jsTestLog("minKey and maxKey");
    // Verify these collections match their dbHash on an _id range where the two collections are
    // identical.
    let hashA =
        assert.commandWorked(db.runCommand({dbhash: 1, collections: [coll], minKey: 2, maxKey: 9}));
    let hashB = assert.commandWorked(
        db.runCommand({dbhash: 1, collections: [copyColl], minKey: 2, maxKey: 9}));
    let hashC = assert.commandWorked(
        db.runCommand({dbhash: 1, collections: [subColl], minKey: 2, maxKey: 9}));
    assert.eq(hashA.collections[coll],
              hashB.collections[copyColl],
              `A: ${tojson(hashA)} B: ${tojson(hashB)}`);
    assert.neq(hashA.collections[coll],
               hashC.collections[subColl],
               `A: ${tojson(hashA)} C: ${tojson(hashC)}`);

    hashA =
        assert.commandWorked(db.runCommand({dbhash: 1, collections: [coll], minKey: 2, maxKey: 6}));
    hashC = assert.commandWorked(
        db.runCommand({dbhash: 1, collections: [subColl], minKey: 2, maxKey: 6}));
    assert.eq(hashA.collections[coll],
              hashC.collections[subColl],
              `A: ${tojson(hashA)} C: ${tojson(hashC)}`);

    // Test min > max
    hashA =
        assert.commandWorked(db.runCommand({dbhash: 1, collections: [coll], minKey: 7, maxKey: 6}));
    assert.eq(hashA.collections[coll], NULL_MD5, hashA);

    // Test that minKey is exclusive.
    hashA =
        assert.commandWorked(db.runCommand({dbhash: 1, collections: [coll], minKey: 2, maxKey: 6}));
    hashC = assert.commandWorked(
        db.runCommand({dbhash: 1, collections: [subColl], minKey: 2, maxKey: 6}));
    assert.eq(hashA.collections[coll],
              hashC.collections[subColl],
              `A: ${tojson(hashA)} C: ${tojson(hashC)}`);

    // Test maxKey is inclusive.
    hashA =
        assert.commandWorked(db.runCommand({dbhash: 1, collections: [coll], minKey: 3, maxKey: 7}));
    hashC = assert.commandWorked(
        db.runCommand({dbhash: 1, collections: [subColl], minKey: 3, maxKey: 7}));
    assert.neq(hashA.collections[coll],
               hashC.collections[subColl],
               `A: ${tojson(hashA)} C: ${tojson(hashC)}`);

    jsTestLog("test MinKey and MaxKey values");
    // MinKey is less than the min _id 0.
    hashA = assert.commandWorked(
        db.runCommand({dbhash: 1, collections: [coll], minKey: MinKey, maxKey: 6}));
    hashB =
        assert.commandWorked(db.runCommand({dbhash: 1, collections: [coll], minKey: 0, maxKey: 6}));
    assert.neq(hashA.collections[coll],
               hashB.collections[coll],
               `A: ${tojson(hashA)} B: ${tojson(hashB)}`);

    hashB = assert.commandWorked(
        db.runCommand({dbhash: 1, collections: [coll], minKey: -1, maxKey: 6}));
    assert.eq(hashA.collections[coll],
              hashB.collections[coll],
              `A: ${tojson(hashA)} B: ${tojson(hashB)}`);

    // MaxKey is greater than the max _id 9.
    hashA = assert.commandWorked(
        db.runCommand({dbhash: 1, collections: [coll], minKey: 3, maxKey: MaxKey}));
    hashB =
        assert.commandWorked(db.runCommand({dbhash: 1, collections: [coll], minKey: 3, maxKey: 9}));
    assert.eq(hashA.collections[coll],
              hashB.collections[coll],
              `A: ${tojson(hashA)} B: ${tojson(hashB)}`);

    jsTestLog("test minKey:MinKey when a document's _id is MinKey");
    // Insert document with _id MinKey.
    let collWithMinKey = copyColl;
    const docMin = {_id: MinKey, a: "foo"};
    assert.commandWorked(db[collWithMinKey].insert(docMin));

    // Test docMin is exclusive with `minKey: Minkey` param but is inclusive with no minKey param.
    hashA = assert.commandWorked(
        db.runCommand({dbhash: 1, collections: [collWithMinKey], minKey: MinKey}));
    hashB = assert.commandWorked(db.runCommand({dbhash: 1, collections: [collWithMinKey]}));
    assert.neq(hashA.collections[collWithMinKey],
               hashB.collections[collWithMinKey],
               `A: ${tojson(hashA)} B: ${tojson(hashB)}`);
    assert.commandWorked(db[collWithMinKey].deleteOne(docMin));

    jsTestLog("test maxKey:MaxKey when a document's _id is MaxKey");
    let collWithMaxKey = copyColl;
    const docMax = {_id: MaxKey, a: "bar"};
    assert.commandWorked(db[collWithMaxKey].insert(docMax));
    // Test docMax is inclusive with maxKey: Maxkey param and is also inclusive with no maxKey
    // param.
    hashA = assert.commandWorked(
        db.runCommand({dbhash: 1, collections: [collWithMaxKey], maxKey: MaxKey}));
    hashB = assert.commandWorked(db.runCommand({dbhash: 1, collections: [collWithMaxKey]}));
    assert.eq(hashA.collections[collWithMaxKey],
              hashB.collections[collWithMaxKey],
              `A: ${tojson(hashA)} B: ${tojson(hashB)}`);
    assert.commandWorked(db[collWithMaxKey].deleteOne(docMax));

    jsTestLog("Test mixed _id types");
    let diffColl = copyColl;
    assert.commandWorked(db[diffColl].insert({id: "baa"}));
    assert.commandWorked(db[diffColl].insert({id: "baz"}));

    assert.commandWorked(
        db.runCommand({dbhash: 1, collections: [diffColl], minKey: "baa", maxKey: "baz"}));
    // Test min > max
    hashA = assert.commandWorked(
        db.runCommand({dbhash: 1, collections: [diffColl], minKey: "baz", maxKey: "baa"}));
    assert.eq(hashA.collections[diffColl], NULL_MD5, hashA);

    // Test docDiffType is inclusive with maxKey: MaxKey param.
    hashA =
        assert.commandWorked(db.runCommand({dbhash: 1, collections: [diffColl], maxKey: MaxKey}));
    hashB = assert.commandWorked(db.runCommand({dbhash: 1, collections: [diffColl]}));
    assert.eq(hashA.collections[diffColl],
              hashB.collections[diffColl],
              `A: ${tojson(hashA)} B: ${tojson(hashB)}`);

    // Test docDiffType is inclusive with minKey: MinKey param.
    hashA =
        assert.commandWorked(db.runCommand({dbhash: 1, collections: [diffColl], minKey: MinKey}));
    hashB = assert.commandWorked(db.runCommand({dbhash: 1, collections: [diffColl]}));
    assert.eq(hashA.collections[diffColl],
              hashB.collections[diffColl],
              `A: ${tojson(hashA)} B: ${tojson(hashB)}`);
}

const collName = jsTestName();
// Test dbhash on normal collections.
testDbHash(collName, {});

// Test dbhash on capped collections.
testDbHash(collName + "_capped", {capped: true, size: 4096 * 2});

// Test dbhash on clustered collections.
testDbHash(collName + "_clustered", {clusteredIndex: {key: {_id: 1}, unique: true}});
}());
