/**
 * @tags: [featureFlagSbeFull]
 */
const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");

const db = conn.getDB(jsTestName());

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
    assert.commandFailedWithCode(
        res, [ErrorCodes.BadValue, 51024]);  // getting BadValue when binary is > 7.1, else 51024

    res = db.runCommand({analyze: coll.getName(), key: "a.b", sampleRate: null});
    assert.commandWorked(res);

    res = db.runCommand({analyze: coll.getName(), sampleSize: 123});
    assert.commandFailedWithCode(res, 6799706);

    res = db.runCommand({analyze: coll.getName(), key: "a.b", sampleSize: "hello"});
    assert.commandFailedWithCode(res, ErrorCodes.TypeMismatch);

    res = db.runCommand({analyze: coll.getName(), key: "a.b", sampleSize: -5});
    assert.commandFailedWithCode(
        res, [ErrorCodes.BadValue, 51024]);  // getting BadValue when binary is > 7.1, else 51024

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
    // Verify that we can bucket into numberic, string, and date buckets
    res = db.runCommand({analyze: coll.getName(), key: "a", numberBuckets: 4});
    assert.commandWorked(res);
    assert.eq(4, syscoll.find({_id: "a"})[0].statistics.scalarHistogram.buckets.length);
})();

cleanup();

assert.commandWorked(coll.insert(Array.from(Array(10000), (_, i) => Object.create({a: i}))));
res = db.runCommand({analyze: coll.getName(), key: "a"});
assert.commandWorked(res);
// Assert on default number of buckets
assert.eq(100, syscoll.find({_id: "a"})[0].statistics.scalarHistogram.buckets.length);

cleanup();

(function validateBucketsNumberOfBucketsAndTypes() {
    try {
        // Test CE histogram creation: Number of collection types 1, Number of histogram buckets 1
        // (fail) and 2 (success).
        assert.commandWorked(coll.insert({a: NumberInt(1)}));
        assert.commandWorked(coll.insert({a: NumberInt(2)}));
        assert.commandWorked(coll.insert({a: NumberInt(3)}));
        assert.commandWorked(coll.insert({a: NumberLong(1234)}));
        assert.commandWorked(coll.insert({a: NumberLong(2324)}));
        assert.commandWorked(coll.insert({a: NumberLong(3345)}));
        assert.commandWorked(coll.insert({a: NumberDecimal(365)}));
        assert.commandWorked(coll.insert({a: NumberDecimal(37987)}));

        assert.commandFailedWithCode(
            coll.runCommand({analyze: coll.getName(), key: "a", numberBuckets: 1}), 7299701);
        assert.commandWorked(
            coll.runCommand({analyze: coll.getName(), key: "a", numberBuckets: 2}));

        assert.commandWorked(coll.insert({a: 'a'}));
        assert.commandWorked(coll.insert({a: 'b'}));
        assert.commandWorked(coll.insert({a: 'asdasdafsadfsaasdasdasddf'}));

        // Test CE histogram creation: Number of collection types 2, Number of histogram buckets 2
        // (fail) and 3 (success).
        assert.commandFailedWithCode(
            coll.runCommand({analyze: coll.getName(), key: "a", numberBuckets: 2}), 7299701);
        assert.commandWorked(
            coll.runCommand({analyze: coll.getName(), key: "a", numberBuckets: 3}));

        assert.commandWorked(coll.insert({a: null}));
        assert.commandWorked(coll.insert({a: true}));
        assert.commandWorked(coll.insert({a: [1, 2, 3, 4]}));
        assert.commandWorked(
            coll.insert({a: Timestamp(new Date(Date.UTC(1984, 0, 1)).getTime() / 1000, 0)}));

        // Test CE histogram creation: Number of collection types 3, Number of histogram buckets 2,
        // 3 (fail) and 4 (success).
        assert.commandFailedWithCode(
            coll.runCommand({analyze: coll.getName(), key: "a", numberBuckets: 2}), 7299701);
        assert.commandFailedWithCode(
            coll.runCommand({analyze: coll.getName(), key: "a", numberBuckets: 3}), 7299701);
        assert.commandWorked(
            coll.runCommand({analyze: coll.getName(), key: "a", numberBuckets: 4}));

    } finally {
        // Ensure that query knob doesn't leak into other testcases in the suite.
        cleanup();
    }
})();

cleanup();

MongoRunner.stopMongod(conn);
