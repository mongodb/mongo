// Tests that specifying a maxTimeMS on a getMore request to mongos is not interpreted as a deadline
// for the operationfor a tailable + awaitData cursor.
// This test was designed to reproduce SERVER-33942 against a mongos.
// @tags: [requires_sharding, requires_capped]
(function() {
    "use strict";

    const st = new ShardingTest({shards: 2});

    const db = st.s.getDB("test");
    const coll = db.capped;
    assert.commandWorked(db.runCommand({create: "capped", capped: true, size: 1024}));
    assert.writeOK(coll.insert({}));
    const findResult = assert.commandWorked(
        db.runCommand({find: "capped", filter: {}, tailable: true, awaitData: true}));

    const cursorId = findResult.cursor.id;
    assert.neq(cursorId, 0);

    // Test that the getMores on this tailable cursor are immune to interrupt.
    assert.commandWorked(
        db.adminCommand({configureFailPoint: "maxTimeAlwaysTimeOut", mode: "alwaysOn"}));
    assert.commandWorked(db.runCommand({getMore: cursorId, collection: "capped", maxTimeMS: 30}));
    assert.commandWorked(db.runCommand({getMore: cursorId, collection: "capped"}));
    assert.commandWorked(
        db.adminCommand({configureFailPoint: "maxTimeAlwaysTimeOut", mode: "off"}));

    st.stop();
}());
