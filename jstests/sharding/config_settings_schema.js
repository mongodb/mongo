/**
 * Tests that the schema on config.settings works as intended.
 *
 * @tags: [does_not_support_stepdowns]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

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
// Invalid balancer mode included
assert.commandFailed(
    coll.update({_id: "balancer"}, {_id: "balancer", mode: "bad"}, {upsert: true}));
// Invalid value for stopped
assert.commandFailed(
    coll.update({_id: "balancer"}, {_id: "balancer", mode: "full", stopped: "no"}, {upsert: true}));
// Active window included with only start
assert.commandFailed(coll.update({_id: "balancer"},
                                 {_id: "balancer", mode: "full", activeWindow: {start: "hh.mm"}},
                                 {upsert: true}));
// Extra field in balancer doc
assert.commandFailed(coll.update(
    {_id: "balancer"}, {_id: "balancer", mode: "full", stoppppped: true}, {upsert: true}));

// Updates that match the schema are accepted
// No schema is enforced for automerge and ReadWriteConcernDefaults
assert.commandWorked(coll.update({_id: "automerge"}, {$set: {anything: true}}, {upsert: true}));
assert.commandWorked(
    coll.update({_id: "ReadWriteConcernDefaults"}, {$set: {anything: true}}, {upsert: true}));
// Schema enforces chunksize to be a number (not an int), so doubles will be accepted and the
// balancer will fail until a correct value is set
assert.commandWorked(coll.update({_id: "chunksize"}, {$set: {value: 3.5}}, {upsert: true}));
// Valid integer value
assert.commandWorked(coll.update({_id: "chunksize"}, {$set: {value: 5}}, {upsert: true}));
// Just a valid mode for the balancer is fine
assert.commandWorked(
    coll.update({_id: "balancer"}, {_id: "balancer", mode: "full"}, {upsert: true}));
// Just a valid stopped for the balancer is fine
assert.commandWorked(
    coll.update({_id: "balancer"}, {_id: "balancer", stopped: true}, {upsert: true}));
// Valid mode + valid stopped value
assert.commandWorked(
    coll.update({_id: "balancer"}, {_id: "balancer", mode: "full", stopped: true}, {upsert: true}));
// Valid mode + valid active window
assert.commandWorked(
    coll.update({_id: "balancer"},
                {_id: "balancer", mode: "full", activeWindow: {start: "00.00", stop: "06.00"}},
                {upsert: true}));
// Valid mode, stopped, and active window
assert.commandWorked(coll.update(
    {_id: "balancer"},
    {_id: "balancer", mode: "full", stopped: true, activeWindow: {start: "00.00", stop: "06.00"}},
    {upsert: true}));
// Valid mode + bool for secondary throttle
assert.commandWorked(coll.update(
    {_id: "balancer"}, {_id: "balancer", mode: "full", _secondaryThrottle: true}, {upsert: true}));
// Valid mode + object for secondary throttle
assert.commandWorked(
    coll.update({_id: "balancer"},
                {_id: "balancer", mode: "full", _secondaryThrottle: {"w": "majority"}},
                {upsert: true}));
// Valid mode + wait for delete + allow jumbo chunks
assert.commandWorked(coll.update(
    {_id: "balancer"},
    {_id: "balancer", mode: "full", _secondaryThrottle: true, attemptToBalanceJumboChunks: false},
    {upsert: true}));

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
