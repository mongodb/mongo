// Test that a change stream is able to survive an upgrade. This is the most basic test to
// demonstrate the survival of a stream, presuming the driver will attempt to retry and resume the
// stream after network errors.
// @tags: [uses_change_streams]
(function() {
"use strict";

load("jstests/multiVersion/libs/multi_rs.js");  // For upgradeSet.
load("jstests/replsets/rslib.js");              // For startSetIfSupportsReadMajority.

const rst = new ReplSetTest({
    nodes: 2,
    nodeOptions: {binVersion: "last-stable"},
});

if (!startSetIfSupportsReadMajority(rst)) {
    jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
    rst.stopSet();
    return;
}

rst.initiate();

let testDB = rst.getPrimary().getDB(jsTestName());
let coll = testDB.change_stream_upgrade;

// Open a change stream against a 4.0 binary. We will use the resume token from this stream to
// resume the stream once the set has been upgraded.
let streamStartedOnOldVersion = coll.watch();
assert.commandWorked(coll.insert({_id: "first insert, just for resume token"}));

assert.soon(() => streamStartedOnOldVersion.hasNext());
let change = streamStartedOnOldVersion.next();
assert.eq(change.operationType, "insert", tojson(change));
assert.eq(change.documentKey._id, "first insert, just for resume token", tojson(change));
const resumeTokenFromLastStable = change._id;

assert.commandWorked(coll.insert({_id: "before binary upgrade"}));
// Upgrade the set to the new binary version, but keep the feature compatibility version at 4.0.
rst.upgradeSet({binVersion: "latest"});
testDB = rst.getPrimary().getDB(jsTestName());
coll = testDB.change_stream_upgrade;

// Test that we can resume the stream on the new binaries.
streamStartedOnOldVersion = coll.watch([], {resumeAfter: resumeTokenFromLastStable});
assert.soon(() => streamStartedOnOldVersion.hasNext());
change = streamStartedOnOldVersion.next();
assert.eq(change.operationType, "insert", tojson(change));
assert.eq(change.documentKey._id, "before binary upgrade", tojson(change));

let streamStartedOnNewVersionOldFCV = coll.watch();

assert.commandWorked(coll.insert({_id: "after binary upgrade, before fcv switch"}));

let resumeTokenFromNewVersionOldFCV;
[streamStartedOnOldVersion, streamStartedOnNewVersionOldFCV].forEach(stream => {
    assert.soon(() => stream.hasNext());
    change = stream.next();
    assert.eq(change.operationType, "insert", tojson(change));
    assert.eq(change.documentKey._id, "after binary upgrade, before fcv switch", tojson(change));
    if (resumeTokenFromNewVersionOldFCV === undefined) {
        resumeTokenFromNewVersionOldFCV = change._id;
    } else {
        assert.eq(resumeTokenFromNewVersionOldFCV, change._id);
    }
});

// Explicitly set feature compatibility version to 4.2.
assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: "4.2"}));

const streamStartedOnNewVersion = coll.watch();

// Test that we can still resume with the token from the old version. We should see the same
// document again.
streamStartedOnOldVersion = coll.watch([], {resumeAfter: resumeTokenFromLastStable});
assert.soon(() => streamStartedOnOldVersion.hasNext());
change = streamStartedOnOldVersion.next();
assert.eq(change.operationType, "insert", tojson(change));
assert.eq(change.documentKey._id, "before binary upgrade", tojson(change));

assert.soon(() => streamStartedOnOldVersion.hasNext());
change = streamStartedOnOldVersion.next();
assert.eq(change.operationType, "insert", tojson(change));
assert.eq(change.documentKey._id, "after binary upgrade, before fcv switch", tojson(change));

assert.commandWorked(coll.insert({_id: "after fcv upgrade"}));
const resumedStreamOnNewVersion = coll.watch([], {resumeAfter: resumeTokenFromNewVersionOldFCV});

// Test that all open streams continue to produce change events, and that the newly resumed
// stream sees the write that just happened since it comes after the resume token used.
for (let stream of [streamStartedOnOldVersion,
                    streamStartedOnNewVersionOldFCV,
                    streamStartedOnNewVersion,
                    resumedStreamOnNewVersion]) {
    assert.soon(() => stream.hasNext());
    change = stream.next();
    assert.eq(change.operationType, "insert", tojson(change));
    assert.eq(change.documentKey._id, "after fcv upgrade", tojson(change));
    stream.close();
}

rst.stopSet();
}());
