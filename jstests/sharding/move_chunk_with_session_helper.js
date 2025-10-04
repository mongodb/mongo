import {awaitRSClientHosts} from "jstests/replsets/rslib.js";

/**
 * High level test scenario:
 * 1. Shard collection.
 * 2. Perform writes.
 * 3. Migrate only chunk to other shard.
 * 4. Retry writes.
 * 5. Step up secondary and wait for new primary.
 * 6. Retry writes.
 * 7. Migrate only chunk back to original shard.
 * 8. Retry writes.
 */
export var testMoveChunkWithSession = function (
    st,
    collName,
    cmdObj,
    setupFunc,
    checkRetryResultFunc,
    checkDocumentsFunc,
    useAdminCommand = false,
) {
    let ns = "test." + collName;
    let testDB = st.s.getDB("test");
    let coll = testDB.getCollection(collName);

    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));

    setupFunc(coll);
    let result = assert.commandWorked(useAdminCommand ? st.s.adminCommand(cmdObj) : testDB.runCommand(cmdObj));

    jsTestLog("MOVECHUNK");
    assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 0}, to: st.shard1.shardName}));

    checkRetryResultFunc(
        result,
        assert.commandWorked(useAdminCommand ? st.s.adminCommand(cmdObj) : testDB.runCommand(cmdObj)),
    );
    checkDocumentsFunc(coll);

    const secondary = st.rs1.getSecondary();
    st.rs1.stepUp(secondary);

    st.rs1.awaitNodesAgreeOnPrimary();
    st.configRS.nodes.concat([st.s]).forEach(function awaitNode(conn) {
        awaitRSClientHosts(conn, {host: st.rs1.getPrimary().host}, {ok: true, ismaster: true});
    });

    checkRetryResultFunc(
        result,
        assert.commandWorked(useAdminCommand ? st.s.adminCommand(cmdObj) : testDB.runCommand(cmdObj)),
    );
    checkDocumentsFunc(coll);

    // Make sure that the other shard knows about the latest primary.
    awaitRSClientHosts(st.rs0.getPrimary(), {host: st.rs1.getPrimary().host}, {ok: true, ismaster: true});
    assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 0}, to: st.shard0.shardName}));

    checkRetryResultFunc(
        result,
        assert.commandWorked(useAdminCommand ? st.s.adminCommand(cmdObj) : testDB.runCommand(cmdObj)),
    );
    checkDocumentsFunc(coll);
};
