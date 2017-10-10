load("jstests/replsets/rslib.js");

/**
 * High level test scenario:
 * 1. Shard collection.
 * 2. Perform writes.
 * 3. Migrate only chunk to other shard.
 * 4. Retry writes.
 * 5. Step down primary and wait for new primary.
 * 6. Retry writes.
 * 7. Migrate only chunk back to original shard.
 * 8. Retry writes.
 */
var testMoveChunkWithSession = function(
    st, collName, cmdObj, setupFunc, checkRetryResultFunc, checkDocumentsFunc) {
    var ns = 'test.' + collName;
    var testDB = st.s.getDB('test');
    var coll = testDB.getCollection(collName);

    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));

    setupFunc(coll);
    var result = assert.commandWorked(testDB.runCommand(cmdObj));

    assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 0}, to: st.shard1.shardName}));

    checkRetryResultFunc(result, assert.commandWorked(testDB.runCommand(cmdObj)));
    checkDocumentsFunc(coll);

    try {
        st.rs1.getPrimary().adminCommand({replSetStepDown: 60, secondaryCatchUpPeriodSecs: 30});
    } catch (excep) {
        print('Expected exception due to step down: ' + tojson(excep));
    }

    st.rs1.awaitNodesAgreeOnPrimary();
    st.configRS.nodes.concat([st.s]).forEach(function awaitNode(conn) {
        awaitRSClientHosts(conn, {host: st.rs1.getPrimary().host}, {ok: true, ismaster: true});
    });

    checkRetryResultFunc(result, assert.commandWorked(testDB.runCommand(cmdObj)));
    checkDocumentsFunc(coll);

    // Make sure that the other shard knows about the latest primary.
    awaitRSClientHosts(
        st.rs0.getPrimary(), {host: st.rs1.getPrimary().host}, {ok: true, ismaster: true});
    assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 0}, to: st.shard0.shardName}));

    checkRetryResultFunc(result, assert.commandWorked(testDB.runCommand(cmdObj)));
    checkDocumentsFunc(coll);
};
