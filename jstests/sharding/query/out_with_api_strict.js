/**
 * Tests that the $out stage executes correctly with apiStrict checking.
 * @tags: [
 *   requires_fcv_70,
 * ]
 */
(function() {
"use strict";

const st = new ShardingTest({shards: 2, rs: {nodes: 1}});
const sourceDB = st.s.getDB("test");
const sourceColl = sourceDB.source;
const targetColl = sourceDB.target;

sourceColl.drop();

st.shardColl(sourceColl, {_id: 1}, {_id: 0}, {_id: 1}, sourceDB.getName());

for (let i = 0; i < 10; i++) {
    assert.commandWorked(sourceColl.insert({_id: i}));
}

targetColl.drop();

// This command uses the internal field `temp` of the create command, which should now pass with
// `apiStrict: true`.
let result = sourceDB.runCommand({
    aggregate: sourceColl.getName(),
    pipeline: [{$out: targetColl.getName()}],
    cursor: {},
    apiVersion: "1",
    apiStrict: true
});
assert.commandWorked(result);

st.stop();
}());
