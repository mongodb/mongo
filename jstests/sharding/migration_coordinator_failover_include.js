load("jstests/libs/fail_point_util.js");
load('jstests/libs/parallel_shell_helpers.js');

function getNewNs(dbName) {
    if (typeof getNewNs.counter == 'undefined') {
        getNewNs.counter = 0;
    }
    getNewNs.counter++;
    const collName = "ns" + getNewNs.counter;
    return [collName, dbName + "." + collName];
}

function runMoveChunkMakeDonorStepDownAfterFailpoint(st,
                                                     dbName,
                                                     failpointName,
                                                     shouldMakeMigrationFailToCommitOnConfig,
                                                     expectAbortDecisionWithCode) {
    const [collName, ns] = getNewNs(dbName);
    jsTest.log("Running migration, making donor step down after failpoint " + failpointName +
               "; shouldMakeMigrationFailToCommitOnConfig is " +
               shouldMakeMigrationFailToCommitOnConfig + "; expectAbortDecisionWithCode is " +
               expectAbortDecisionWithCode + "; ns is " + ns);

    // Wait for mongos to see a primary node on the primary shard, because mongos does not retry
    // writes on NotPrimary errors, and we are about to insert docs through mongos.
    awaitRSClientHosts(st.s, st.rs0.getPrimary(), {ok: true, ismaster: true});

    // Insert some docs into the collection so that the migration leaves orphans on either the
    // donor or recipient, depending on the decision.
    const numDocs = 1000;
    var bulk = st.s.getDB(dbName).getCollection(collName).initializeUnorderedBulkOp();
    for (var i = 0; i < numDocs; i++) {
        bulk.insert({_id: i});
    }
    assert.commandWorked(bulk.execute());

    // Shard the collection.
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

    if (shouldMakeMigrationFailToCommitOnConfig) {
        // Turn on a failpoint to make the migration commit fail on the config server.
        assert.commandWorked(st.configRS.getPrimary().adminCommand(
            {configureFailPoint: "migrationCommitVersionError", mode: "alwaysOn"}));
    }

    jsTest.log("Run the moveChunk asynchronously and wait for " + failpointName + " to be hit.");
    let failpointHandle = configureFailPoint(st.rs0.getPrimary(), failpointName);
    const awaitResult = startParallelShell(
        funWithArgs(function(ns, toShardName, expectAbortDecisionWithCode) {
            if (expectAbortDecisionWithCode) {
                assert.commandFailedWithCode(
                    db.adminCommand({moveChunk: ns, find: {_id: 0}, to: toShardName}),
                    expectAbortDecisionWithCode);
            } else {
                assert.commandWorked(
                    db.adminCommand({moveChunk: ns, find: {_id: 0}, to: toShardName}));
            }
        }, ns, st.shard1.shardName, expectAbortDecisionWithCode), st.s.port);
    failpointHandle.wait();

    jsTest.log("Make the donor primary step down.");
    assert.commandWorked(
        st.rs0.getPrimary().adminCommand({replSetStepDown: 10 /* stepDownSecs */, force: true}));
    failpointHandle.off();

    jsTest.log("Allow the moveChunk to finish.");
    awaitResult();

    if (expectAbortDecisionWithCode) {
        jsTest.log("Expect abort decision, so wait for recipient to clean up the orphans.");
        assert.soon(() => {
            return 0 === st.rs1.getPrimary().getDB(dbName).getCollection(collName).count();
        });

    } else {
        jsTest.log("Expect commit decision, so wait for donor to clean up the orphans.");
        assert.soon(() => {
            return 0 === st.rs0.getPrimary().getDB(dbName).getCollection(collName).count();
        });
    }

    // Wait for mongos to see a new primary of rs0 before running the count command, because mongos
    // will only wait 20 seconds to see a new primary from within the count command, and it may take
    // longer for a new primary to be elected if both replica set nodes run for election at the same
    // time (and therefore both lose the first election).
    awaitRSClientHosts(st.s, st.rs0.getPrimary(), {ok: true, ismaster: true});

    // The data should still be present on the shard that owns the chunk.
    assert.eq(numDocs, st.s.getDB(dbName).getCollection(collName).count());

    jsTest.log("Wait for the donor to delete the migration coordinator doc");
    assert.soon(() => {
        return 0 ===
            st.rs0.getPrimary().getDB("config").getCollection("migrationCoordinators").count();
    });

    if (shouldMakeMigrationFailToCommitOnConfig) {
        // Turn off the failpoint on the config server before returning.
        assert.commandWorked(st.configRS.getPrimary().adminCommand(
            {configureFailPoint: "migrationCommitVersionError", mode: "off"}));
    }
}
