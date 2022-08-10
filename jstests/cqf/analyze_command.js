(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const coll = db.cqf_analyze;
coll.drop();

assert.commandWorked(coll.insert({a: [1, 2, 4, 4, 5, 6, {b: 1}]}));

let res = null;

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

    // Correct error thrown under cqf flag
    res = db.runCommand({analyze: coll.getName()});
    assert.commandFailedWithCode(res, ErrorCodes.NotImplemented);
})();

(function validateKey() {
    res = db.runCommand({analyze: coll.getName(), key: ""});
    assert.commandFailedWithCode(res, 6799703);

    res = db.runCommand({analyze: coll.getName(), key: "a..b"});
    assert.commandFailedWithCode(res, 15998);

    res = db.runCommand({analyze: coll.getName(), key: "a.$b"});
    assert.commandFailedWithCode(res, 16410);

    res = db.runCommand({analyze: coll.getName(), key: "a.0.b"});
    assert.commandFailedWithCode(res, 6799704);

    res = db.runCommand({analyze: coll.getName(), key: "a.b"});
    assert.commandFailedWithCode(res, ErrorCodes.NotImplemented);
})();

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
    assert.commandFailedWithCode(res, ErrorCodes.NotImplemented);

    res = db.runCommand({analyze: coll.getName(), sampleSize: 123});
    assert.commandFailedWithCode(res, 6799706);

    res = db.runCommand({analyze: coll.getName(), key: "a.b", sampleSize: "hello"});
    assert.commandFailedWithCode(res, ErrorCodes.TypeMismatch);

    res = db.runCommand({analyze: coll.getName(), key: "a.b", sampleSize: -5});
    assert.commandFailedWithCode(res, 51024);

    res = db.runCommand({analyze: coll.getName(), key: "a.b", sampleSize: null});
    assert.commandFailedWithCode(res, ErrorCodes.NotImplemented);
})();

// Test API Strict
res = db.runCommand({analyze: coll.getName(), apiVersion: "1", apiStrict: true});
assert.commandFailedWithCode(res, ErrorCodes.APIStrictError);

// Test write concern
res = db.runCommand({analyze: coll.getName(), writeConcern: {w: 1}});
assert.commandFailedWithCode(res, ErrorCodes.NotImplemented);
}());
