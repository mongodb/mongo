// Test profiling of insert, update, delete with write conflicts (SERVER-51456).
//
// @tags: [
//   assumes_write_concern_unchanged,
//   does_not_support_stepdowns,
//   requires_profiling,
// ]

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");  // for 'configureFailPoint()'
load("jstests/libs/profiler.js");         // for 'getLatestProfilerEntry()'

if (jsTestOptions().storageEngine !== "ephemeralForTest") {
    jsTestLog("This test requires ephemeralForTest - exiting");
    return 0;
}

const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());
const coll = testDB.getCollection("test");
const doc = {
    a: 1,
    b: 1
};
const writeConflicts = 100;

assert.commandWorked(testDB.setProfilingLevel(2));

{
    // Test insert.
    assert.commandWorked(testDB.createCollection(coll.getName()));
    const fp = configureFailPoint(db, "EFTThrowWCEOnMerge", {}, {times: writeConflicts});
    assert.commandWorked(testDB.runCommand(
        {insert: coll.getName(), documents: [doc], comment: jsTestName() + "-insert"}));
    fp.off();

    const profileObj =
        getLatestProfilerEntry(testDB, {"command.comment": jsTestName() + "-insert"});

    assert.eq(profileObj.ninserted, 1, profileObj);
    assert.eq(profileObj.keysInserted, 1, profileObj);
    assert.gt(profileObj.writeConflicts, 0, profileObj);
}

{
    // Test delete.
    coll.drop();
    assert.commandWorked(coll.insert(doc));
    const fp = configureFailPoint(db, "EFTThrowWCEOnMerge", {}, {times: writeConflicts});
    assert.commandWorked(testDB.runCommand({
        delete: coll.getName(),
        deletes: [{q: doc, limit: 0}],
        comment: jsTestName() + "-delete"
    }));
    fp.off();

    const profileObj =
        getLatestProfilerEntry(testDB, {"command.comment": jsTestName() + "-delete"});

    assert.eq(profileObj.ndeleted, 1, profileObj);
    assert.eq(profileObj.keysDeleted, 1, profileObj);
    assert.gt(profileObj.writeConflicts, 0, profileObj);
}

{
    // Test update.
    coll.drop();
    assert.commandWorked(coll.insert(doc));
    assert.commandWorked(coll.createIndex({a: 1}));
    const fp = configureFailPoint(db, "EFTThrowWCEOnMerge", {}, {times: writeConflicts});
    assert.commandWorked(testDB.runCommand({
        update: coll.getName(),
        updates: [{q: {a: 1}, u: {$set: {c: 1}, $inc: {a: -10}}}],
        comment: jsTestName() + "-update"
    }));
    fp.off();

    const profileObj =
        getLatestProfilerEntry(testDB, {"command.comment": jsTestName() + "-update"});

    assert.eq(profileObj.keysInserted, 1, profileObj);
    assert.eq(profileObj.keysDeleted, 1, profileObj);
    assert.eq(profileObj.nMatched, 1, profileObj);
    assert.eq(profileObj.nModified, 1, profileObj);
    assert.gt(profileObj.writeConflicts, 0, profileObj);
}

{
    // Test upsert - update.
    coll.drop();
    assert.commandWorked(coll.insert(doc));
    assert.commandWorked(coll.createIndex({a: 1}));
    const fp = configureFailPoint(db, "EFTThrowWCEOnMerge", {}, {times: writeConflicts});
    assert.commandWorked(testDB.runCommand({
        update: coll.getName(),
        updates: [{q: {a: 1}, u: {$set: {c: 1}, $inc: {a: -10}}, upsert: true}],
        comment: jsTestName() + "-upsertu"
    }));
    fp.off();

    const profileObj =
        getLatestProfilerEntry(testDB, {"command.comment": jsTestName() + "-upsertu"});

    assert.eq(profileObj.keysInserted, 1, profileObj);
    assert.eq(profileObj.keysDeleted, 1, profileObj);
    assert.eq(profileObj.nMatched, 1, profileObj);
    assert.eq(profileObj.nModified, 1, profileObj);
    assert.eq(profileObj.nUpserted, 0, profileObj);
    assert.gt(profileObj.writeConflicts, 0, profileObj);
}

{
    // Test upsert - insert.
    coll.drop();
    assert.commandWorked(coll.insert(doc));
    assert.commandWorked(coll.createIndex({a: 1}));
    const fp = configureFailPoint(db, "EFTThrowWCEOnMerge", {}, {times: writeConflicts});
    assert.commandWorked(testDB.runCommand({
        update: coll.getName(),
        updates: [{q: {a: 2}, u: {$set: {c: 1}, $inc: {a: -10}}, upsert: true}],
        comment: jsTestName() + "-upserti"
    }));
    fp.off();

    const profileObj =
        getLatestProfilerEntry(testDB, {"command.comment": jsTestName() + "-upserti"});

    assert.eq(profileObj.keysInserted, 2, profileObj);
    assert.eq(profileObj.nMatched, 0, profileObj);
    assert.eq(profileObj.nModified, 0, profileObj);
    assert.eq(profileObj.nUpserted, 1, profileObj);
    assert.gt(profileObj.writeConflicts, 0, profileObj);
}

{
    // Test findAndModify - delete.
    coll.drop();
    assert.commandWorked(coll.insert(doc));
    const fp = configureFailPoint(db, "EFTThrowWCEOnMerge", {}, {times: writeConflicts});
    assert.commandWorked(testDB.runCommand({
        findAndModify: coll.getName(),
        query: doc,
        remove: true,
        comment: jsTestName() + "-fnmdel"
    }));
    fp.off();

    const profileObj =
        getLatestProfilerEntry(testDB, {"command.comment": jsTestName() + "-fnmdel"});

    assert.eq(profileObj.ndeleted, 1, profileObj);
    assert.eq(profileObj.keysDeleted, 1, profileObj);
    assert.gt(profileObj.writeConflicts, 0, profileObj);
}

{
    // Test findAndModify - update.
    coll.drop();
    assert.commandWorked(coll.insert(doc));
    assert.commandWorked(coll.createIndex({a: 1}));
    const fp = configureFailPoint(db, "EFTThrowWCEOnMerge", {}, {times: writeConflicts});
    assert.commandWorked(testDB.runCommand({
        findAndModify: coll.getName(),
        query: doc,
        update: {$set: {c: 1}, $inc: {a: -10}},
        comment: jsTestName() + "-fnmupd"
    }));
    fp.off();

    const profileObj =
        getLatestProfilerEntry(testDB, {"command.comment": jsTestName() + "-fnmupd"});

    assert.eq(profileObj.keysInserted, 1, profileObj);
    assert.eq(profileObj.keysDeleted, 1, profileObj);
    assert.eq(profileObj.nMatched, 1, profileObj);
    assert.eq(profileObj.nModified, 1, profileObj);
    assert.gt(profileObj.writeConflicts, 0, profileObj);
}

{
    // Test findAndModify - upsert - update.
    coll.drop();
    assert.commandWorked(coll.insert(doc));
    assert.commandWorked(coll.createIndex({a: 1}));
    const fp = configureFailPoint(db, "EFTThrowWCEOnMerge", {}, {times: writeConflicts});
    assert.commandWorked(testDB.runCommand({
        findAndModify: coll.getName(),
        query: doc,
        update: {$set: {c: 1}, $inc: {a: -10}},
        upsert: true,
        comment: jsTestName() + "-fnmupsu"
    }));
    fp.off();

    const profileObj =
        getLatestProfilerEntry(testDB, {"command.comment": jsTestName() + "-fnmupsu"});

    assert.eq(profileObj.keysInserted, 1, profileObj);
    assert.eq(profileObj.keysDeleted, 1, profileObj);
    assert.eq(profileObj.nMatched, 1, profileObj);
    assert.eq(profileObj.nModified, 1, profileObj);
    assert.eq(profileObj.nUpserted, 0, profileObj);
    assert.gt(profileObj.writeConflicts, 0, profileObj);
}

{
    // Test findAndModify - upsert - insert.
    coll.drop();
    assert.commandWorked(coll.insert(doc));
    assert.commandWorked(coll.createIndex({a: 1}));
    const fp = configureFailPoint(db, "EFTThrowWCEOnMerge", {}, {times: writeConflicts});
    assert.commandWorked(testDB.runCommand({
        findAndModify: coll.getName(),
        query: {a: 2},
        update: {$set: {c: 1}, $inc: {a: -10}},
        upsert: true,
        comment: jsTestName() + "-fnmupsi"
    }));
    fp.off();

    const profileObj =
        getLatestProfilerEntry(testDB, {"command.comment": jsTestName() + "-fnmupsi"});

    assert.eq(profileObj.keysInserted, 2, profileObj);
    assert.eq(profileObj.nMatched, 0, profileObj);
    assert.eq(profileObj.nModified, 0, profileObj);
    assert.eq(profileObj.nUpserted, 1, profileObj);
    assert.gt(profileObj.writeConflicts, 0, profileObj);
}
})();
