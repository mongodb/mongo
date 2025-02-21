/*
 * Tests the sessions collection has the correct chunk size and autosplit settings while
 * transitioning between FCV.
 */

(function() {
"use strict";

const sessionsNS = "config.system.sessions";
const expectedDefaultChunkSize = 200000;
const customChunkSize = 200000000;

function expectSettings(mongos, chunkSize, autoSplitDisabled) {
    let sessionCollectionDoc =
        mongos.getDB("config").getCollection("collections").findOne({_id: sessionsNS});
    assert.eq(sessionCollectionDoc.maxChunkSizeBytes, chunkSize);
    assert.eq(sessionCollectionDoc.noAutoSplit, autoSplitDisabled);
}

function setSettings(mongos, chunkSize, autoSplitDisabled) {
    let modificationDoc = {};
    if (chunkSize != undefined) {
        modificationDoc.maxChunkSizeBytes = chunkSize;
    }
    if (autoSplitDisabled != undefined) {
        modificationDoc.noAutoSplit = autoSplitDisabled;
    }
    assert.commandWorked(
        mongos.getDB("config").getCollection("collections").updateOne({_id: sessionsNS}, {
            $set: modificationDoc
        }));
}

function dropConfigSystemSettings(mongosConn, configConn, shardConn) {
    let uuid =
        mongosConn.getDB('config').getCollection("collections").findOne({_id: sessionsNS}).uuid;
    assert.commandWorked(
        mongosConn.getDB('config').getCollection("chunks").deleteMany({uuid: uuid}));
    assert.commandWorked(
        mongosConn.getDB('config').getCollection("collections").deleteMany({_id: sessionsNS}));
    configConn.getDB("config").getCollection("system.sessions").drop();
    shardConn.getDB("config").getCollection("system.sessions").drop();
}

const st = new ShardingTest({shards: 1});

// Trigger a refresh to create and shard sessionsNS.
assert.commandWorked(st.s.adminCommand({refreshLogicalSessionCacheNow: 1}));

// The session collection should be created with the default maxChunkSize and auto splitting
// disabled on FCV 6.0.
expectSettings(st.s, expectedDefaultChunkSize, true);

assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: lastContinuousFCV}));

// Expect the chunk size and the no auto split field removed.
expectSettings(st.s, undefined, undefined);

setSettings(st.s, customChunkSize);

expectSettings(st.s, customChunkSize, undefined);

assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

// Upgrading with an already set chunk size should result in no change to config.system.sessions
// settings.
expectSettings(st.s, customChunkSize, undefined);

setSettings(st.s, undefined, true);

expectSettings(st.s, customChunkSize, true);

assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: lastContinuousFCV}));

// Downgrading with an already set chunk size should result in no change to config.system.sessions
// settings.
expectSettings(st.s, customChunkSize, true);

jsTest.log("Dropping config.system.sessions");
dropConfigSystemSettings(st.s, st.configRS.getPrimary(), st.rs0.getPrimary());

// The session collection should be recreated without an explicit maxChunkSize or autosplit settings
// on FCV < 6.0.
jsTest.log("Sending refresh command");
assert.commandWorked(st.configRS.getPrimary().adminCommand({refreshLogicalSessionCacheNow: 1}));
expectSettings(st.s, undefined, undefined);

assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

dropConfigSystemSettings(st.s, st.configRS.getPrimary(), st.rs0.getPrimary());

// The session collection should be recreated with the default maxChunkSize and auto splitting
// disabled on FCV 6.0.
assert.commandWorked(st.configRS.getPrimary().adminCommand({refreshLogicalSessionCacheNow: 1}));
expectSettings(st.s, expectedDefaultChunkSize, true);

assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

// Everything should be removed upon donwgrade to 5.0 even with non-default settings
expectSettings(st.s, undefined, undefined);

st.stop();
})();
