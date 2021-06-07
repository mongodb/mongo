
/**
 * Tests that setting the getLastErrorDefaults field will not cause an error during multi-document
 * transactions.
 */
(function() {
"use strict";
function testReplSet() {
    const replTest =
        new ReplSetTest({nodes: 1, settings: {getLastErrorDefaults: {w: 'majority', wtimeout: 0}}});
    replTest.startSet();
    replTest.initiate();
    const primary = replTest.getPrimary();
    const db = primary.getDB("test");

    const session = primary.startSession();
    const sessionDb = session.getDatabase("test");

    assert.commandWorked(db.runCommand({create: "mycoll"}));

    session.startTransaction();
    assert.commandWorked(sessionDb.mycoll.insert({}));
    session.commitTransaction();
    session.endSession();
    replTest.stopSet();
}

function testSharding() {
    const st = new ShardingTest({
        shards: {
            rs0: {nodes: 1, settings: {getLastErrorDefaults: {w: 'majority', wtimeout: 0}}},
            rs1: {nodes: 1, settings: {getLastErrorDefaults: {w: 'majority', wtimeout: 0}}}
        },
    });

    const db = st.getDB("test");
    const session = st.s.startSession();
    const sessionDb = session.getDatabase("test");

    assert.commandWorked(db.runCommand({create: "mycoll"}));

    session.startTransaction();
    assert.commandWorked(sessionDb.mycoll.insert({}));
    session.commitTransaction();
    session.endSession();
    st.stop();
}

testReplSet();
testSharding();
}());
