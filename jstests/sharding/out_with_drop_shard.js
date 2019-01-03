// Tests that the $out aggregation stage is resilient to drop shard in both the source and
// output collection during execution.
(function() {
    'use strict';

    const st = new ShardingTest({shards: 2, rs: {nodes: 1}});
    // We need the balancer to remove a shard.
    st.startBalancer();

    const mongosDB = st.s.getDB(jsTestName());
    const sourceColl = mongosDB["source"];
    const targetColl = mongosDB["target"];

    function setAggHang(mode) {
        assert.commandWorked(st.shard0.adminCommand(
            {configureFailPoint: "hangWhileBuildingDocumentSourceOutBatch", mode: mode}));
        assert.commandWorked(st.shard1.adminCommand(
            {configureFailPoint: "hangWhileBuildingDocumentSourceOutBatch", mode: mode}));
    }

    function removeShard(shardName) {
        var res = st.s.adminCommand({removeShard: shardName});
        assert.commandWorked(res);
        assert.eq('started', res.state);
        assert.soon(function() {
            res = st.s.adminCommand({removeShard: shardName});
            assert.commandWorked(res);
            return ('completed' === res.state);
        }, "removeShard never completed for shard " + shardName);
    }

    function addShard(shard) {
        assert.commandWorked(st.s.adminCommand({addShard: shard}));
        assert.commandWorked(st.s.adminCommand(
            {moveChunk: sourceColl.getFullName(), find: {shardKey: 0}, to: shard}));
    }
    function runOutWithMode(outMode, shardedColl, dropShard) {
        // Set the failpoint to hang in the first call to DocumentSourceCursor's getNext().
        setAggHang("alwaysOn");

        let comment = outMode + "_" + shardedColl.getName() + "_1";
        let outFn = `
            const sourceDB = db.getSiblingDB(jsTestName());
            const sourceColl = sourceDB["${sourceColl.getName()}"];
            let cmdRes = sourceDB.runCommand({
                aggregate: "${sourceColl.getName()}",
                pipeline: [{$out: {to: "${targetColl.getName()}", mode: "${outMode}"}}],
                cursor: {},
                comment: "${comment}"
            });
            assert.commandWorked(cmdRes);
        `;

        // Start the $out aggregation in a parallel shell.
        let outShell = startParallelShell(outFn, st.s.port);

        // Wait for the parallel shell to hit the failpoint.
        assert.soon(
            () => mongosDB
                      .currentOp({
                          $or: [
                              {op: "command", "command.comment": comment},
                              {op: "getmore", "cursor.originatingCommand.comment": comment}
                          ]
                      })
                      .inprog.length >= 1,
            () => tojson(mongosDB.currentOp().inprog));

        if (dropShard) {
            removeShard(st.shard0.shardName);
        } else {
            addShard(st.rs0.getURL());
        }
        // Unset the failpoint to unblock the $out and join with the parallel shell.
        setAggHang("off");
        outShell();

        // Verify that the $out succeeded.
        assert.eq(2, targetColl.find().itcount());

        assert.commandWorked(targetColl.remove({}));
    }

    // Shard the source collection with shard key {shardKey: 1} and split into 2 chunks.
    st.shardColl(sourceColl.getName(), {shardKey: 1}, {shardKey: 0}, false, mongosDB.getName());

    // Shard the output collection with shard key {shardKey: 1} and split into 2 chunks.
    st.shardColl(targetColl.getName(), {shardKey: 1}, {shardKey: 0}, false, mongosDB.getName());

    // Write two documents in the source collection that should target the two chunks in the target
    // collection.
    assert.commandWorked(sourceColl.insert({shardKey: -1}));
    assert.commandWorked(sourceColl.insert({shardKey: 1}));

    // Note that mode "replaceCollection" is not supported with an existing sharded output
    // collection.
    runOutWithMode("insertDocuments", targetColl, true);
    runOutWithMode("insertDocuments", targetColl, false);
    runOutWithMode("replaceDocuments", targetColl, true);

    st.stop();
})();
