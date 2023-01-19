(function() {
"use strict";

load("jstests/libs/sbe_util.js");  // For checkSBEEnabled.

const conn = MongoRunner.runMongod({setParameter: {featureFlagCommonQueryFramework: true}});
assert.neq(null, conn, "mongod was unable to start up");

const db = conn.getDB(jsTestName());

if (checkSBEEnabled(db, ["featureFlagSbeFull"], true)) {
    jsTestLog("Skipping the test because it doesn't work in Full SBE...");
    MongoRunner.stopMongod(conn);
    return;
}

assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "tryBonsai"}));

const coll = db.cqf_analyze;
const syscoll = db.system.statistics.cqf_analyze;

function cleanup() {
    coll.drop();
    syscoll.drop();
}

function setup() {
    cleanup();
    assert.commandWorked(coll.insert({a: [1, 2, 4, 4, 5, 6, {b: 10}, {b: 7}, {b: 1}]}));
    assert.commandWorked(coll.insert({a: [1, 2, 4, 4, 5, 6, {b: 5}]}));
}

let res = null;

setup();

(function validateNamespace() {
    res = db.runCommand({analyze: ""});
    assert.commandFailedWithCode(res, ErrorCodes.InvalidNamespace);

    res = db.runCommand({analyze: "hello"});
    assert.commandFailedWithCode(res, 6799700);

    const view = db.cqf_analyze_view;
    view.drop();
    assert.commandWorked(db.createView(view.getName(), coll.getName(), []));
    res = db.runCommand({analyze: view.getName()});
    assert.commandFailedWithCode(res, ErrorCodes.CommandNotSupportedOnView);

    const ts = db.cqf_analyze_timeseries;
    ts.drop();
    const timeField = "tm";
    assert.commandWorked(db.createCollection(ts.getName(), {timeseries: {timeField: timeField}}));
    res = db.runCommand({analyze: ts.getName()});
    assert.commandFailedWithCode(res, ErrorCodes.CommandNotSupportedOnView);

    const capped = db.cqf_analyze_capped;
    capped.drop();
    assert.commandWorked(db.createCollection(capped.getName(), {capped: true, size: 256}));
    res = db.runCommand({analyze: capped.getName()});
    assert.commandFailedWithCode(res, 6799701);

    const system_profile = db.system.profile;
    system_profile.drop();
    assert.commandWorked(db.createCollection(system_profile.getName()));
    res = db.runCommand({analyze: system_profile.getName()});
    assert.commandFailedWithCode(res, 6799702);

    // Works correctly when there's a normal collection.
    res = db.runCommand({analyze: coll.getName()});
    assert.commandWorked(res);
})();

setup();

(function validateKey() {
    res = db.runCommand({analyze: coll.getName(), key: ""});
    assert.commandFailedWithCode(res, 6799703);

    res = db.runCommand({analyze: coll.getName(), key: "a..b"});
    assert.commandFailedWithCode(res, 15998);

    res = db.runCommand({analyze: coll.getName(), key: "a.$b"});
    assert.commandFailedWithCode(res, 16410);

    res = db.runCommand({analyze: coll.getName(), key: "a.0.b"});
    assert.commandFailedWithCode(res, 6799704);

    const testAnalayzeValidKey = (keyPath, docs) => {
        coll.drop();
        syscoll.drop();

        // Populate with documents.
        coll.insertMany(docs);

        // Check the stats collection is created, data is inserted, and the index is created.
        const key = keyPath.join('.');
        res = db.runCommand({analyze: coll.getName(), key: key});
        assert.commandWorked(res);
        assert.eq(syscoll.find({_id: key}).count(), 1);
    };
    // Single document single path component.
    testAnalayzeValidKey(["a"], [{a: 1}]);
    // Single document complex path component.
    testAnalayzeValidKey(["a", "b"], [{a: {b: 1}}]);
    // Multiple documents, values missing.
    testAnalayzeValidKey(["a"], [{a: 1}, {b: 1}, {a: 2}]);
})();

setup();

(function validateSampleRateAndSize() {
    res = db.runCommand({analyze: coll.getName(), key: "a.b", sampleRate: 0.1, sampleSize: 1000});
    assert.commandFailedWithCode(res, 6799705);

    res = db.runCommand({analyze: coll.getName(), sampleRate: 0.1});
    assert.commandFailedWithCode(res, 6799706);

    res = db.runCommand({analyze: coll.getName(), key: "a.b", sampleRate: "hello"});
    assert.commandFailedWithCode(res, ErrorCodes.TypeMismatch);

    res = db.runCommand({analyze: coll.getName(), key: "a.b", sampleRate: 1.5});
    assert.commandFailedWithCode(res, 51024);

    res = db.runCommand({analyze: coll.getName(), key: "a.b", sampleRate: null});
    assert.commandWorked(res);

    res = db.runCommand({analyze: coll.getName(), sampleSize: 123});
    assert.commandFailedWithCode(res, 6799706);

    res = db.runCommand({analyze: coll.getName(), key: "a.b", sampleSize: "hello"});
    assert.commandFailedWithCode(res, ErrorCodes.TypeMismatch);

    res = db.runCommand({analyze: coll.getName(), key: "a.b", sampleSize: -5});
    assert.commandFailedWithCode(res, 51024);

    res = db.runCommand({analyze: coll.getName(), key: "a.b", sampleSize: null});
    assert.commandWorked(res);
})();

// Test API Strict
res = db.runCommand({analyze: coll.getName(), apiVersion: "1", apiStrict: true});
assert.commandFailedWithCode(res, ErrorCodes.APIStrictError);

// Test write concern
res = db.runCommand({analyze: coll.getName(), writeConcern: {w: 1}});
assert.commandWorked(res);

cleanup();

assert.commandWorked(coll.insert([
    {a: 1},
    {a: 1.5},
    {a: NumberDecimal("2.1")},
    {a: "string"},
    {a: ISODate("2023-01-18T20:09:36.325Z")},
]));

(function validateBuckets() {
    for (let i = 0; i <= 2; i++) {
        res = db.runCommand({analyze: coll.getName(), key: "a", numberBuckets: i});
        assert.commandFailed(res);
    }
    // TODO SERVER-72997: Fix and enable tests
    // Verify that we can bucket into numberic, string, and date buckets
    // res = db.runCommand({analyze: coll.getName(), key: "a", numberBuckets: 3});
    // assert.commandWorked(res);
    res = db.runCommand({analyze: coll.getName(), key: "a", numberBuckets: 5});
    assert.commandWorked(res);
    assert.eq(5, syscoll.find({_id: "a"})[0].statistics.scalarHistogram.buckets.length);
})();

cleanup();

assert.commandWorked(coll.insert(Array.from(Array(10000), (_, i) => Object.create({a: i}))));
res = db.runCommand({analyze: coll.getName(), key: "a"});
assert.commandWorked(res);
// Assert on default number of buckets
assert.eq(100, syscoll.find({_id: "a"})[0].statistics.scalarHistogram.buckets.length);

cleanup();

MongoRunner.stopMongod(conn);
}());
