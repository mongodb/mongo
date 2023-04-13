/**
 * Tests that the schema on config.settings works as intended.
 *
 * @tags: [does_not_support_stepdowns]
 */
(function() {
'use strict';

load("jstests/libs/feature_flag_util.js");

var st = new ShardingTest({shards: 1, config: 2});

let coll = st.config.settings;

// Updates that violate schema are rejected
// Chunk size too small
assert.commandFailed(coll.update({_id: "chunksize"}, {$set: {value: -1}}, {upsert: true}));
// Chunk size must be a number
assert.commandFailed(coll.update({_id: "chunksize"}, {$set: {value: "string"}}, {upsert: true}));
// Chunk size too big
assert.commandFailed(coll.update({_id: "chunksize"}, {$set: {value: 5000}}, {upsert: true}));
// Extra field in chunk size doc
assert.commandFailed(
    coll.update({_id: "chunksize"}, {$set: {value: 100, extraField: 10}}, {upsert: true}));
// Not a valid setting _id
assert.commandFailed(coll.update({_id: "notARealSetting"}, {$set: {value: 10}}, {upsert: true}));

// Updates that match the schema are accepted
// No schema is enforced for balancer, automerge, and ReadWriteConcernDefaults
assert.commandWorked(coll.update({_id: "balancer"}, {$set: {anything: true}}, {upsert: true}));
if (FeatureFlagUtil.isEnabled(st.config, "AutoMerger")) {
    assert.commandWorked(coll.update({_id: "automerge"}, {$set: {anything: true}}, {upsert: true}));
}
assert.commandWorked(
    coll.update({_id: "ReadWriteConcernDefaults"}, {$set: {anything: true}}, {upsert: true}));
// Schema enforces chunksize to be a number (not an int), so doubles will be accepted and the
// balancer will fail until a correct value is set
assert.commandWorked(coll.update({_id: "chunksize"}, {$set: {value: 3.5}}, {upsert: true}));
// Valid integer value
assert.commandWorked(coll.update({_id: "chunksize"}, {$set: {value: 5}}, {upsert: true}));

// User cannot change schema on config.settings
assert.commandFailedWithCode(
    st.s.getDB("config").runCommand({"collMod": "settings", "validator": {}}),
    ErrorCodes.InvalidOptions);
assert.commandFailedWithCode(
    st.s.getDB("config").runCommand({"collMod": "settings", "validationLevel": "off"}),
    ErrorCodes.InvalidOptions);
assert.commandFailedWithCode(
    st.s.getDB("config").runCommand({"collMod": "settings", "validationAction": "warn"}),
    ErrorCodes.InvalidOptions);

st.stop();
})();
