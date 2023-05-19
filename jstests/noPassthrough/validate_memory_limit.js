/**
 * Test that the memory usage of validate is properly limited according to the
 * maxValidateMemoryUsageMB parameter.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_wiredtiger,
 * ]
 */
(function() {
"use strict";

load("jstests/disk/libs/wt_file_helper.js");

const kIndexKeyLength = 1024 * 1024;

const baseName = "validate_memory_limit";
const dbpath = MongoRunner.dataPath + baseName + "/";
let conn = MongoRunner.runMongod({dbpath: dbpath});
let coll = conn.getDB("test").getCollection("corrupt");

function corruptIndex() {
    const uri = getUriForIndex(coll, "_id_");
    conn = truncateUriAndRestartMongod(uri, conn);
    coll = conn.getDB("test").getCollection("corrupt");
}

function checkValidate(maxMemoryUsage, {minMissingKeys, maxMissingKeys}) {
    conn.getDB("test").adminCommand({setParameter: 1, maxValidateMemoryUsageMB: maxMemoryUsage});
    const res = coll.validate();
    assert.commandWorked(res);
    assert(!res.valid, tojson(res));
    const notAllReportedPrefix =
        "Not all index entry inconsistencies are reported due to memory limitations.";
    assert.containsPrefix(notAllReportedPrefix, res.errors, tojson(res));
    assert.gte(res.missingIndexEntries.length, minMissingKeys, tojson(res));
    assert.lte(res.missingIndexEntries.length, maxMissingKeys, tojson(res));
}

function checkValidateLogs() {
    assert(checkLog.checkContainsWithAtLeastCountJson(
        conn, 7463100, {"spec": {"v": 2, "key": {"_id": 1}, "name": "_id_"}}, 1));
}

function checkValidateRepair() {
    const res = coll.validate({repair: true});
    assert.commandWorked(res);
    assert(!res.valid, tojson(res));
    assert(res.repaired, tojson(res));
}

// Insert a document with a key larger than maxValidateMemoryUsageMB and test that we still report
// at least one inconsistency.
const indexKey = "a".repeat(kIndexKeyLength);
assert.commandWorked(coll.insert({_id: indexKey}));
corruptIndex();
checkValidate(1, {minMissingKeys: 1, maxMissingKeys: 1});
checkValidateLogs();

// Can't repair successfully if there aren't any index inconsistencies reported.
checkValidateRepair();

// Clear collection between tests.
coll.drop();

// Test that if we have keys distributed across many buckets, and would exceed
// maxValidateMemoryUsageMB, we report as many inconsistencies as we can.
for (let i = 0; i < 10; ++i) {
    const indexKey = i.toString().repeat(kIndexKeyLength / 5);
    assert.commandWorked(coll.insert({_id: indexKey}));
}

corruptIndex();
// If each key is maxMem/5, then we can keep 4 of them (the 5th would put us at the limit). However,
// each key is counted twice, so realistically we only expect to track 2 of them. However, there's
// a small chance we could get hash collisions that would lead to us reporting only 1.
checkValidate(1, {minMissingKeys: 1, maxMissingKeys: 2});
checkValidateLogs();

// Repair, but incompletely if only some inconsistencies are reported.
checkValidateRepair();

MongoRunner.stopMongod(conn, null, {skipValidation: true});
})();
