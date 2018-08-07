/*
 * Shared functions and constants between index_bigkeys.js and index_bigkeys_background.js
 */
"use strict";

var createCheckFunc = function(keyPattern) {
    return (testColl, numItems, numIndexes) => {
        assert(testColl.validate().valid);
        assert.eq(numItems, testColl.count());
        // Find by index
        var c = testColl.find({k: keyPattern}).count();
        assert.eq(numItems, c);
        assert.eq(numIndexes, testColl.getIndexes().length);
    };
};

function doInsert(collName, keys, expectDupError = false) {
    for (let i = 0; i < keys.length; i++) {
        let res = db.runCommand({insert: collName, documents: [{_id: i, k: keys[i], c: 0}]});
        if (!expectDupError)
            assert.commandWorked(res);
        else
            assert.commandFailedWithCode(res, ErrorCodes.DuplicateKey);
    }
}

function doUpdate(collName, keys) {
    for (let key of keys) {
        let res = db.runCommand({update: collName, updates: [{q: {k: key}, u: {$inc: {c: 1}}}]});
        assert.commandWorked(res);
        assert.eq(1, res.nModified);
    }
}

function doDelete(collName, keys) {
    for (let key of keys) {
        let res = db.runCommand({delete: collName, deletes: [{q: {k: key}, limit: 0}]});
        assert.commandWorked(res);
        assert.eq(1, res.n);
    }
}

function runTest(createIndexOpts, keys, checkFunc) {
    const collName = "big_keys_index_test";
    const testColl = db[collName];

    // 1. Insert documents when index exists.
    testColl.drop();
    assert.commandWorked(testColl.createIndex({k: 1}, createIndexOpts));
    doInsert(collName, keys);
    checkFunc(testColl, keys.length, 2);

    // Make sure unique index works.
    if ("unique" in createIndexOpts) {
        doInsert(collName, keys, true);
        checkFunc(testColl, keys.length, 2);
    }

    // 2. Reindex when documents exist.
    assert.commandWorked(testColl.dropIndex({k: 1}));
    assert.commandWorked(testColl.createIndex({k: 1}, createIndexOpts));
    checkFunc(testColl, keys.length, 2);

    // 3. Create the index when documents exist.
    testColl.drop();
    doInsert(collName, keys);
    assert.eq(1, testColl.getIndexes().length);
    assert.commandWorked(testColl.createIndex({k: 1}, createIndexOpts));
    checkFunc(testColl, keys.length, 2);

    // 4. Update the documents.
    doUpdate(collName, keys);
    checkFunc(testColl, keys.length, 2);

    // 5. Remove all the documents.
    doDelete(collName, keys);
    checkFunc(testColl, 0, 2);

    // 6. Drop the index when documents exist.
    doInsert(collName, keys);
    assert.commandWorked(db.runCommand({dropIndexes: collName, index: {k: 1}}));
    checkFunc(testColl, keys.length, 1);
}

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
var bigStringCheck = createCheckFunc(/^a/);

// Case 2: Document containing large array as key
var docArrayKeys = (() => {
    let keys = [];
    // {..., k: {a : [0,1,2, ... ,9999] } }
    keys.push({a: Array.apply(null, {length: 10000}).map(Number.call, Number)});
    return keys;
})();
var docArrayCheck = createCheckFunc(docArrayKeys[0]);

// Case 3: Array containing large array as key
var arrayArrayKeys = (() => {
    let keys = [];
    // {..., k: [ [0,1,2, ... ,9999] ] }
    keys.push([Array.apply(null, {length: 10000}).map(Number.call, Number)]);
    return keys;
})();
var arrayArrayCheck = createCheckFunc(arrayArrayKeys[0]);
