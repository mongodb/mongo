// Test that certain basic commands preserve write concern errors.
// @tags: [ requires_fcv_49, ]
//

(function() {
"use strict";

load("jstests/sharding/libs/failpoint_helpers.js");

let st = new ShardingTest({shards: 1, config: 1});

// Test drop collection
{
    let db = st.s.getDB("test");
    // _shardsvrDropCollectionParticipant required for PM-1965
    failCommandsWithWriteConcernError(st.rs0, ["drop", "_shardsvrDropCollectionParticipant"]);
    assert.commandWorked(db.collection.insert({"a": 1}));
    assert.commandFailedWithCode(db.runCommand({drop: "collection"}), 12345);
    turnOffFailCommand(st.rs0);
}

// Test drop database
{
    let db = st.s.getDB("test");
    // _shardsvrDropDatabaseParticipant required for PM-1965
    failCommandsWithWriteConcernError(st.rs0, ["dropDatabase", "_shardsvrDropDatabaseParticipant"]);
    assert.commandWorked(db.collection.insert({"a": 1}));
    assert.commandFailedWithCode(db.runCommand({"dropDatabase": 1}), 12345);
    turnOffFailCommand(st.rs0);
}

st.stop();
})();