// Test that certain basic commands preserve write concern errors.
// @tags: [ requires_fcv_49, ]
//

(function() {
"use strict";

load("jstests/sharding/libs/failpoint_helpers.js");

let st = new ShardingTest({shards: 1, config: 1});

{
    let db = st.s.getDB("test");
    failCommandsWithWriteConcernError(st.rs0, ["drop", "_shardsvrDropCollectionParticipant"]);
    assert.commandWorked(db.collection.insert({"a": 1}));
    assert.commandFailedWithCode(db.runCommand({drop: "collection"}), 12345);
}

st.stop();
})();