// Tests that special stages which must run on mongos cannot be run in combination with an $out
// stage.
(function() {
    "use strict";

    const st = new ShardingTest({shards: 2, rs: {nodes: 1}, config: 1});
    const db = st.s0.getDB("db");
    const admin = st.s0.getDB("admin");

    // Create a collection in the db to get around optimizations that will do nothing in lieu of
    // failing when the db is empty.
    assert.commandWorked(db.runCommand({create: "coll"}));

    // These should fail because the initial stages require mongos execution and $out requires
    // shard execution.
    assert.commandFailedWithCode(
        db.runCommand(
            {aggregate: 1, pipeline: [{$listLocalSessions: {}}, {$out: "test"}], cursor: {}}),
        ErrorCodes.IllegalOperation);
    assert.commandFailedWithCode(
        admin.runCommand(
            {aggregate: 1, pipeline: [{$currentOp: {localOps: true}}, {$out: "test"}], cursor: {}}),
        ErrorCodes.IllegalOperation);
    assert.commandFailedWithCode(
        db.runCommand({aggregate: 1, pipeline: [{$changeStream: {}}, {$out: "test"}], cursor: {}}),
        ErrorCodes.IllegalOperation);

    st.stop();
}());
