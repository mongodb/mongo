/**
 * Tests that $merge doesn't fail when a non-default readConcern is
 * set on the session.
 * @tags: [requires_fcv_71]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2, rs: {nodes: 1}});

const mongosDB = st.s0.getDB("merge_nondefault_read_concern");

assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));

const baseMergeCommand = {
    aggregate: "source",
    pipeline:
        [{$merge: {into: "target", on: "_id", whenMatched: "replace", whenNotMatched: "insert"}}],
    cursor: {},
};

// Test with command level override.
var withReadConcern = baseMergeCommand;
withReadConcern.readConcern = {
    level: "majority"
};
assert.commandWorked(mongosDB.runCommand(withReadConcern));

// Test with global override.
assert.commandWorked(
    mongosDB.adminCommand({"setDefaultRWConcern": 1, "defaultReadConcern": {level: "majority"}}));
assert.commandWorked(mongosDB.runCommand(baseMergeCommand));

st.stop();
