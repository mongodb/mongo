// Tests that special stages which must run on router cannot be run in combination with an $out or
// $merge stage.
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2, rs: {nodes: 1}});
const db = st.s0.getDB("db");

// Create a collection in the db to get around optimizations that will do nothing in lieu of
// failing when the db is empty.
assert.commandWorked(db.runCommand({create: "coll"}));

// These should fail because the initial stages require router execution and $out/$merge
// requires shard execution.
assert.commandFailedWithCode(
    db.runCommand({aggregate: 1, pipeline: [{$listLocalSessions: {}}, {$out: "test"}], cursor: {}}),
    ErrorCodes.IllegalOperation,
);
assert.commandFailedWithCode(
    db.runCommand({
        aggregate: "coll",
        pipeline: [{$_internalSplitPipeline: {mergeType: st.getMergeType(db)}}, {$out: "test"}],
        cursor: {},
    }),
    ErrorCodes.IllegalOperation,
);
assert.commandFailedWithCode(
    db.runCommand({aggregate: 1, pipeline: [{$changeStream: {}}, {$out: "test"}], cursor: {}}),
    ErrorCodes.IllegalOperation,
);
assert.commandFailedWithCode(
    db.runCommand({aggregate: 1, pipeline: [{$listLocalSessions: {}}, {$merge: {into: "test"}}], cursor: {}}),
    ErrorCodes.IllegalOperation,
);
assert.commandFailedWithCode(
    db.runCommand({
        aggregate: "coll",
        pipeline: [{$_internalSplitPipeline: {mergeType: st.getMergeType(db)}}, {$merge: {into: "test"}}],
        cursor: {},
    }),
    ErrorCodes.IllegalOperation,
);
assert.commandFailedWithCode(
    db.runCommand({aggregate: 1, pipeline: [{$changeStream: {}}, {$merge: {into: "test"}}], cursor: {}}),
    ErrorCodes.IllegalOperation,
);

st.stop();
