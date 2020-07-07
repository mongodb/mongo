/*
 * Helper functions for testing big index keys.
 */
"use strict";

///////////////////////////////////////////////////////////////////////////////

// Case 1: Big string as key
var bigStringKeys = (() => {
    let keys = [];
    var str = "aaaabbbbccccddddeeeeffffgggghhhh";
    while (str.length < 20000) {
        keys.push(str);
        str = str + str;
    }
    return keys;
})();

// Case 2: Document containing large array as key
var docArrayKeys = (() => {
    let keys = [];
    // {..., k: {a : [0,1,2, ... ,9999] } }
    keys.push({a: Array.apply(null, {length: 10000}).map(Number.call, Number)});
    return keys;
})();

// Case 3: Array containing large array as key
var arrayArrayKeys = (() => {
    let keys = [];
    // {..., k: [ [0,1,2, ... ,9999] ] }
    keys.push([Array.apply(null, {length: 10000}).map(Number.call, Number)]);
    return keys;
})();

var bigKeyGroups = [bigStringKeys, docArrayKeys, arrayArrayKeys];
var bigKeyPatterns = [/^a/, docArrayKeys[0], arrayArrayKeys[0]];

///////////////////////////////////////////////////////////////////////////////

function readIndexKeysByQueryDocs(testColl, keyPattern, numFoundExpected) {
    assert(testColl.validate().valid);
    assert.eq(numFoundExpected, testColl.find({k: keyPattern}).count());
}

function insertIndexKeysByInsertDocs(testColl, keys) {
    for (let i = 0; i < keys.length; i++) {
        assert.commandWorked(testColl.insert({k: keys[i]}));
    }
}

function insertIndexKeysByUpdateDocs(testDB, collName, keys) {
    for (let i = 0; i < keys.length; i++) {
        assert.commandWorked(testDB[collName].insert({_id: i, k: i}));
    }
    for (let i = 0; i < keys.length; i++) {
        let res = testDB.runCommand(
            {update: collName, updates: [{q: {_id: i}, u: {$set: {k: keys[i]}}}]});
        assert.commandWorked(res);
        assert.eq(1, res.nModified);
    }
}

function deleteIndexKeysByRemoveDocs(testDB, collName, keys) {
    for (let key of keys) {
        let res = testDB.runCommand({delete: collName, deletes: [{q: {k: key}, limit: 1}]});
        assert.commandWorked(res);
        assert.eq(1, res.n);
    }
}

function deleteIndexKeysByUpdateDocs(testDB, collName, keys) {
    for (let i = 0; i < keys.length; i++) {
        let res =
            testDB.runCommand({update: collName, updates: [{q: {k: keys[i]}, u: {$set: {k: i}}}]});
        assert.commandWorked(res);
        assert.eq(1, res.nModified);

        // Delete the doc to make the collection clean.
        assert.commandWorked(testDB[collName].remove({k: i}));
    }
}

function createIndex(testColl, unique) {
    let numIndexesBefore = testColl.getIndexes().length;
    assert.commandWorked(testColl.createIndex({k: 1}, {unique: unique}));
    assert.eq(numIndexesBefore + 1, testColl.getIndexes().length);
}

function dropIndex(testColl) {
    let numIndexesBefore = testColl.getIndexes().length;
    assert.commandWorked(testColl.dropIndex({k: 1}));
    assert.eq(numIndexesBefore - 1, testColl.getIndexes().length);
}
///////////////////////////////////////////////////////////////////////////////

/**
 * This test makes sure we can insert, read, update and delete big index keys.
 */
function testAllInteractionsWithBigIndexKeys(testDB, collName) {
    [true, false].forEach(function(uniqueIndex) {
        [true, false].forEach(function(createIndexFirst) {
            for (let i = 0; i < bigKeyGroups.length; i++) {
                let keys = bigKeyGroups[i];
                let keyPattern = bigKeyPatterns[i];
                testDB[collName].drop();
                assert.commandWorked(testDB.runCommand({create: collName}));
                let testColl = testDB[collName];

                // Test that we can insert big index keys.
                if (createIndexFirst) {
                    createIndex(testColl, uniqueIndex);
                    insertIndexKeysByInsertDocs(testColl, keys);
                } else {
                    insertIndexKeysByInsertDocs(testColl, keys);
                    createIndex(testColl, uniqueIndex);
                }

                // Test that we can read big index keys by querying with keys.
                readIndexKeysByQueryDocs(testColl, keyPattern, keys.length);

                // Test that we can delete big index keys by removing the documents.
                deleteIndexKeysByRemoveDocs(testDB, collName, keys);

                // Insert big index keys again, this time by updating documents with bigger indexed
                // value.
                insertIndexKeysByUpdateDocs(testDB, collName, keys);

                // Test that we can delete big index keys by updating the documents with smaller
                // indexed value.
                deleteIndexKeysByUpdateDocs(testDB, collName, keys);

                // Test dropIndex.
                dropIndex(testColl);
            }
        });
    });
    testDB[collName].drop();
}
