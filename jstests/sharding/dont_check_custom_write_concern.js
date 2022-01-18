/**
 * Tests that we don't check the correctness of setting the default write concern to a custom write
 * concern in sharded clusters, but that the success of subsequent writes depends on whether the
 * write concern actually exists on the shards.
 */

(function() {
'use strict';

// Define the custom write concern multiRegion, only in the shard.
const st = new ShardingTest({
    name: "dont_check_custom_write_concern",
    shards: {
        rs0: {
            nodes: [
                {rsConfig: {tags: {region: "us"}}},
                {rsConfig: {tags: {region: "us"}}},
                {rsConfig: {tags: {region: "eu"}}}
            ],
            settings: {getLastErrorModes: {multiRegion: {region: 2}}}
        }
    }
});

const coll = st.s.getDB("db").getCollection("test_dont_check_custom_write_concern_coll");
assert.commandWorked(coll.insert({a: 1}));

// Ensure that setting the write concern to nonexistent custom write concerns (i.e. in this case,
// strings that are not "majority") will succeed, but subsequent writes will fail.
assert.commandWorked(st.s.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: "1"}}));
assert.commandFailedWithCode(coll.insert({a: 1}), ErrorCodes.UnknownReplWriteConcern);

assert.commandWorked(
    st.s.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: "bajority"}}));
assert.commandFailedWithCode(coll.insert({a: 1}), ErrorCodes.UnknownReplWriteConcern);

// Ensure that after setting the write concern validly, subsequent writes will succeed.
assert.commandWorked(
    st.s.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: "majority"}}));
assert.commandWorked(coll.insert({a: 1}));

// Ensure that setting the write concern to a valid custom write concern that exists on the single
// shard will succeed, and subsequent writes will succeed.
assert.commandWorked(
    st.s.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: "multiRegion"}}));
assert.commandWorked(coll.insert({a: 1}));

st.stop();
})();
