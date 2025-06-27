/**
 * Verifies that setting the default write concern to a custom value in sharded clusters
 * succeeds only if the value is defined on the CSRS and all shards.
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {reconfig} from "jstests/replsets/rslib.js";

const st = new ShardingTest({
    name: "custom_write_concern_test",
    shards: {
        rs0: {
            nodes: [
                {rsConfig: {tags: {region: "us"}}},
                {rsConfig: {tags: {region: "us"}}},
                {rsConfig: {tags: {region: "eu"}}}
            ],
            settings: {getLastErrorModes: {multiRegion: {region: 2}, singleRegion: {region: 1}}}
        },
        rs1: {
            nodes: [
                {rsConfig: {tags: {region: "asia"}}},
                {rsConfig: {tags: {region: "us"}}},
                {rsConfig: {tags: {region: "eu"}}}
            ],
            settings: {getLastErrorModes: {multiRegion: {region: 3}}}
        }
    }
});

const coll = st.s.getDB("db").getCollection("test_custom_write_concern_coll");
assert.commandWorked(coll.insert({a: 1}));

// Ensure that after setting the write concern validly, subsequent writes will succeed.
assert.commandWorked(
    st.s.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: "majority"}}));
assert.commandWorked(coll.insert({a: 1}));

// Ensure that setting the write concern to a nonexistent custom value will fail.
assert.commandFailedWithCode(
    st.s.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: "1"}}),
    ErrorCodes.UnknownReplWriteConcern);
assert.commandFailedWithCode(
    st.s.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: "bajority"}}),
    ErrorCodes.UnknownReplWriteConcern);

if (!TestData.configShard) {
    // Ensure that setting the write concern to a custom value that exists on some/all shards,
    // but does not exist on the config server fails.
    assert.commandFailedWithCode(
        st.s.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: "multiRegion"}}),
        ErrorCodes.UnknownReplWriteConcern);
    assert.commandFailedWithCode(
        st.s.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: "singleRegion"}}),
        ErrorCodes.UnknownReplWriteConcern);
}

// Reconfigure CSRS
var cfg = st.configRS.getReplSetConfigFromNode();
for (let i = 0; i < cfg.members.length; i++) {
    if (i % 2) {
        cfg.members[i].tags = {region: "us", dc: "west"};
    } else {
        cfg.members[i].tags = {region: "asia", dc: "east"};
    }
}
cfg.settings.getLastErrorModes = {
    singleRegion: {region: 1},
    multiRegion: {region: 2},
    multiDC: {dc: 2}
};
reconfig(st.configRS, cfg);

// Ensure that setting the write concern to a custom value that exists only on the CSRS fails.
assert.commandFailedWithCode(
    st.s.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: "multiDC"}}),
    ErrorCodes.UnknownReplWriteConcern);

// Ensure that setting the write concern to a custom value that exists on the CSRS,
// but is not present on all shards fails.
assert.commandFailedWithCode(
    st.s.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: "singleRegion"}}),
    ErrorCodes.UnknownReplWriteConcern);

// Ensure that setting the write concern to a custom value that exists on all shards and the
// CSRS works, and subsequent writes succeed.
assert.commandWorked(
    st.s.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: "multiRegion"}}));
assert.commandWorked(coll.insert({a: 1}));

st.stop();
