/**
 * Test that validate detects and properly reports multikey inconsistencies.
 */
(function() {
"use strict";

const baseName = "validate_multikey_failures";
const dbpath = MongoRunner.dataPath + baseName + "/";
let conn = MongoRunner.runMongod({dbpath: dbpath});
let coll = conn.getDB("test").getCollection("corrupt");

const resetCollection = () => {
    coll.drop();
    coll.createIndex({"a.b": 1});
};

const disableMultikeyUpdate = () => {
    assert.commandWorked(
        conn.adminCommand({configureFailPoint: "skipUpdateIndexMultikey", mode: "alwaysOn"}));
};

const enableMultikeyUpdate = () => {
    assert.commandWorked(
        conn.adminCommand({configureFailPoint: "skipUpdateIndexMultikey", mode: "off"}));
};

function checkValidateLogs() {
    assert(checkLog.checkContainsWithAtLeastCountJson(
        conn, 7463100, {"spec": {"v": 2, "key": {"_id": 1}, "name": "_id_"}}, 1));
}

// Test that multiple keys suggest index should be marked multikey.
resetCollection();
disableMultikeyUpdate();
assert.commandWorked(coll.insert({a: {b: [1, 2]}}));
enableMultikeyUpdate();
let res = coll.validate();
assert.commandWorked(res);
assert(!res.valid);
assert.eq(res.indexDetails["a.b_1"].errors.length, 1);
assert(res.indexDetails["a.b_1"].errors[0].startsWith("Index a.b_1 is not multikey"));
assert(res.indexDetails["a.b_1"].errors[0].includes("2 key(s)"));
assert(checkLog.checkContainsWithAtLeastCountJson(conn, 7556100, {"indexName": "a.b_1"}, 1));
assert(checkLog.checkContainsWithAtLeastCountJson(conn, 7556101, {"indexKey": {"a.b": 1}}, 1));
assert(checkLog.checkContainsWithAtLeastCountJson(conn, 7556101, {"indexKey": {"a.b": 2}}, 1));

// Test that a single-entry array suggests index should be marked multikey.
resetCollection();
disableMultikeyUpdate();
assert.commandWorked(coll.insert({a: {b: [3]}}));
enableMultikeyUpdate();
res = coll.validate();
assert.commandWorked(res);
assert(!res.valid);
assert.eq(res.indexDetails["a.b_1"].errors.length, 1);
assert(res.indexDetails["a.b_1"].errors[0].startsWith("Index a.b_1 is not multikey"));
assert(res.indexDetails["a.b_1"].errors[0].includes("1 key(s)"));
assert(checkLog.checkContainsWithAtLeastCountJson(conn, 7556100, {"indexName": "a.b_1"}, 1));
assert(checkLog.checkContainsWithAtLeastCountJson(conn, 7556101, {"indexKey": {"a.b": 3}}, 1));

// Test that a mis-match in multikey paths should be marked multikey.
resetCollection();
assert.commandWorked(coll.insert({a: [{b: 4}, {b: 5}]}));
disableMultikeyUpdate();
assert.commandWorked(coll.insert({a: {b: [6]}}));
enableMultikeyUpdate();
res = coll.validate();
assert.commandWorked(res);
assert(!res.valid);
assert.eq(res.indexDetails["a.b_1"].errors.length, 1);
assert(res.indexDetails["a.b_1"].errors[0].startsWith(
    "Index a.b_1 multikey paths do not cover a document"));
assert(checkLog.checkContainsWithAtLeastCountJson(conn, 7556100, {"indexName": "a.b_1"}, 1));
assert(checkLog.checkContainsWithAtLeastCountJson(conn, 7556101, {"indexKey": {"a.b": 6}}, 1));
assert(checkLog.checkContainsWithAtLeastCountJson(conn, 5367500, {"index": "a.b_1"}, 1));

MongoRunner.stopMongod(conn, null, {skipValidation: true});
})();