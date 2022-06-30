/**
 * Tests the behavior of the createIndex and dropIndex events without various command parameters.
 *
 * @tags: [
 *   requires_fcv_60,
 *   assumes_unsharded_collection,
 *   assumes_against_mongod_not_mongos,
 * ]
 */
(function() {
"use strict";

load('jstests/libs/collection_drop_recreate.js');  // For 'assertDropAndRecreateCollection' and
                                                   // 'assertDropCollection'.
load('jstests/libs/change_stream_util.js');        // For 'ChangeStreamTest' and
                                                   // 'assertChangeStreamEventEq'.

const testDB = db.getSiblingDB(jsTestName());

const dbName = testDB.getName();
const collName = jsTestName();
const ns = {
    db: dbName,
    coll: collName
};

const pipeline = [{$changeStream: {showExpandedEvents: true}}];
const cst = new ChangeStreamTest(testDB);

function getCollectionUuid(coll) {
    const collInfo = testDB.getCollectionInfos({name: coll})[0];
    return collInfo.info.uuid;
}

function assertNextChangeEvent(cursor, expectedEvent) {
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: expectedEvent});
}

function runTest(startChangeStream, insertDataBeforeCreateIndex) {
    const cst = new ChangeStreamTest(testDB);

    assert.commandWorked(testDB.runCommand({create: collName}));
    if (insertDataBeforeCreateIndex) {
        testDB[collName].insert({a: 1, b: "j", c: "k", d: "l", e: "m", f: [55.5, 42.3]});
    }

    function testCreateIndexAndDropIndex(key, options, opDescKey = key, dropKey) {
        let name = "";
        if (options.hasOwnProperty("name")) {
            // If the "name" option was explicitly specified, use that
            name = options.name;
            dropKey = name;
        } else {
            // Otherwise, determine what "name" should be based on "key"
            let arr = [];
            for (let property in key) {
                arr.push(property);
                arr.push(key[property]);
            }
            name = arr.join("_");
        }

        if (dropKey == undefined) {
            dropKey = name;
        }

        let cursor = startChangeStream();
        let opDesc = {indexes: [Object.assign({v: 2, key: opDescKey, name: name}, options)]};

        assert.commandWorked(testDB[collName].createIndex(key, options));
        assertNextChangeEvent(
            cursor, {operationType: "createIndexes", ns: ns, operationDescription: opDesc});

        assert.commandWorked(testDB[collName].dropIndexes([dropKey]));
        assertNextChangeEvent(cursor,
                              {operationType: "dropIndexes", ns: ns, operationDescription: opDesc});
    }

    // Test createIndex() with various option followed by dropIndexes("*").
    let options = {
        hidden: true,
        partialFilterExpression: {a: {$gte: 0}},
        expireAfterSeconds: 86400,
        storageEngine: {wiredTiger: {}}
    };
    testCreateIndexAndDropIndex({a: 1}, options);

    // Test createIndex() with a non-simple collation followed by dropIndex(). We include all
    // fields in the collation spec so that we don't rely on any default settings.
    options = {
        collation: {
            locale: "en_US",
            caseLevel: false,
            caseFirst: "off",
            strength: 3,
            numericOrdering: false,
            alternate: "non-ignorable",
            maxVariable: "punct",
            normalization: false,
            backwards: false,
            version: "57.1"
        }
    };
    testCreateIndexAndDropIndex({e: 1}, options, {e: 1}, "*");

    // Test createIndex() for a wildcard index on all fields with the wildcardProjection option,
    // followed by dropIndex().
    options = {name: "wi", wildcardProjection: {a: true, b: true, _id: false}};
    testCreateIndexAndDropIndex({"$**": 1}, options);

    // Test createIndex() for a text index with various options, followed by dropIndex().
    options = {
        name: "text",
        weights: {e: 1},
        default_language: "english",
        language_override: "language",
        textIndexVersion: 3
    };
    testCreateIndexAndDropIndex({e: "text"}, options, {_fts: "text", _ftsx: 1});

    // Test createIndex() for a 2d index with various options, followed by dropIndex().
    options = {name: "2d", min: -150.0, max: 150.0, bits: 26};
    testCreateIndexAndDropIndex({f: "2d"}, options);

    // Test createIndex() for a 2dsphere index with various options, followed by dropIndex().
    options = {name: "2dsphere", "2dsphereIndexVersion": 3};
    testCreateIndexAndDropIndex({f: "2dsphere"}, options);

    // Test createIndexes() to create two sparse indexes (with one index being a compound index),
    // followed by dropIndexes().
    let cursor = startChangeStream();
    let opDesc1 = {indexes: [{v: 2, key: {b: 1, c: -1}, name: "b_1_c_-1", sparse: true}]};
    let opDesc2 = {indexes: [{v: 2, key: {d: "hashed"}, name: "d_hashed", sparse: true}]};
    if (!insertDataBeforeCreateIndex) {
        // If the collection was empty before calling createIndexes(), then there will be a separate
        // change stream event for each index.
        assert.commandWorked(
            testDB[collName].createIndexes([{b: 1, c: -1}, {d: "hashed"}], {sparse: true}));
        cst.assertNextChangesEqualUnordered({
            cursor: cursor,
            expectedChanges: [
                {operationType: "createIndexes", ns: ns, operationDescription: opDesc1},
                {operationType: "createIndexes", ns: ns, operationDescription: opDesc2}
            ]
        });
    } else {
        // If the collection was not empty before calling createIndexes(), then there will be a
        // single change stream event that covers both indexes.
        assert.commandWorked(
            testDB[collName].createIndexes([{b: 1, c: -1}, {d: "hashed"}], {sparse: true}));
        assertNextChangeEvent(cursor, {
            operationType: "createIndexes",
            ns: ns,
            operationDescription: {indexes: [opDesc1.indexes[0], opDesc2.indexes[0]]}
        });
    }
    assert.commandWorked(testDB[collName].dropIndexes(["b_1_c_-1", "d_hashed"]));
    cst.assertNextChangesEqualUnordered({
        cursor: cursor,
        expectedChanges: [
            {operationType: "dropIndexes", ns: ns, operationDescription: opDesc1},
            {operationType: "dropIndexes", ns: ns, operationDescription: opDesc2}
        ]
    });

    testDB[collName].drop();
}

// Run the test using a whole-db change stream on an empty collection.
runTest((() => cst.startWatchingChanges({pipeline, collection: 1})), false);

// Run the test using a single change stream on an empty collection.
runTest((() => cst.startWatchingChanges({pipeline, collection: collName})), false);

// Run the test using a whole-db collection change stream on a non-empty collection.
runTest((() => cst.startWatchingChanges({pipeline, collection: 1})), true);

// Run the test using a single collection change stream on a non-empty collection.
runTest((() => cst.startWatchingChanges({pipeline, collection: collName})), true);
}());
