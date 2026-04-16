/**
 * Test transition to dedicated config server waits for orphanCleanupDelaySecs before dropping
 * local collections.
 * @tags: [
 * requires_fcv_80,
 * # This test disables the range deleter (disableResumableRangeDeleter: true) to test orphan
 * # cleanup delay. Stepdowns during moveChunk create range deletion tasks that can't be cleaned
 * # up, causing subsequent moveChunk retries to fail with "overlapping range deletion" errors.
 * does_not_support_stepdowns
 * ]
 */

function insertDocs(coll, numDocs) {
    const docs = [];
    for (let i = 0; i < numDocs; i++) {
        docs.push({_id: i, data: "x".repeat(10)});
    }
    assert.commandWorked(coll.insertMany(docs));
}

const orphanCleanupDelaySecs = 3;

const st = new ShardingTest({
    name: jsTestName(),
    shards: 2,
    rs: {nodes: 3},
    other: {
        enableBalancer: true,
        configShard: true,
        rsOptions: {
            setParameter: {
                orphanCleanupDelaySecs: orphanCleanupDelaySecs,
                disableResumableRangeDeleter: true,
            },
        },
    },
});

function transitionBackIfNeeded() {
    const numShards = st.s.getDB("config").shards.count();
    if (numShards == 2) {
        jsTest.log("Cancel transition to dedicated config server by unsetting draining flag");
        st.configRS.getPrimary().getDB("config").shards.updateOne(
            {_id: "config", draining: true}, {$unset: {draining: ""}});
        const drainingShards =
            st.s.getDB("config").shards.find({"draining": true}).toArray();
        assert.eq(0, drainingShards.length);
    } else {
        jsTest.log("Transition back to embedded config server");
        assert.commandWorked(st.s.adminCommand({transitionFromDedicatedConfigServer: 1}));
    }
}

// Test 1: Transition completes with delayed range deletion tasks after waiting
(function testTransitionCompletesWithDelayedRangeDeletionTasks() {
    jsTest.log("Test transition completes with delayed range deletion tasks after waiting");
    const dbName = jsTestName() + "_delayed";
    const collName = "testColl";
    const ns = dbName + "." + collName;

    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: "config"}));
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

    const coll = st.s.getDB(dbName).getCollection(collName);
    insertDocs(coll, 100);

    assert.soon(
        () => st.s.adminCommand({split: ns, middle: {_id: 50}}).ok,
        "split did not complete within the timeout",
    );
    assert.soon(
        () => st.s.adminCommand({
                      moveChunk: ns,
                      find: {_id: 0},
                      to: st.shard1.shardName,
                      _waitForDelete: false,
                  })
                  .ok,
        "moveChunk did not complete within the timeout",
    );

    assert.soon(
        () => st.s.adminCommand({
                      moveChunk: ns,
                      find: {_id: 50},
                      to: st.shard1.shardName,
                      _waitForDelete: false,
                  })
                  .ok,
        "moveChunk did not complete within the timeout",
    );

    assert.soon(
        () => st.s.adminCommand({movePrimary: dbName, to: st.shard1.shardName}).ok,
        "movePrimary did not complete within the timeout",
    );

    jsTest.log("Start transition to dedicated config server");
    let res = st.s.adminCommand({transitionToDedicatedConfigServer: 1});
    assert.commandWorked(res);
    assert.eq("started", res.state, "Expected 'started' state: " + tojson(res));
    st.configRS.awaitReplication();

    jsTest.log("Wait for transition to complete (verifying orphan cleanup delay is enforced)");
    let sawPendingDataCleanup = false;
    assert.soonNoExcept(
        () => {
            res = st.s.adminCommand({transitionToDedicatedConfigServer: 1});
            assert.commandWorked(res);
            if (res.state === "pendingDataCleanup") {
                sawPendingDataCleanup = true;
            }
            return res.state === "completed";
        },
        "transitionToDedicatedConfigServer did not reach 'completed' state within the timeout",
        120000,
    );

    assert(
        sawPendingDataCleanup,
        "Expected to observe 'pendingDataCleanup' state while waiting for orphanCleanupDelaySecs");

    const configShard = st.s.getDB("config").shards.findOne({_id: "config"});
    assert.eq(null, configShard, "Config shard should have been removed");

    assert.commandWorked(st.s.getDB(dbName).dropDatabase());

    transitionBackIfNeeded();
})();

// Test 2: Queries started before draining fail with QueryPlanKilled after collection drop
(function testQueriesFailWithQueryPlanKilledAfterDrop() {
    jsTest.log(
        "Test queries started before draining fail with QueryPlanKilled after collection drop");
    const dbName = jsTestName() + "_qryKilled";
    const collName = "testColl";
    const ns = dbName + "." + collName;

    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: "config"}));
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

    const coll = st.s.getDB(dbName).getCollection(collName);
    insertDocs(coll, 100);

    st.configRS.awaitReplication();

    jsTest.log("Opening cursor via mongos with secondary read preference before draining starts");
    const cursor = st.s.getDB(dbName)
                       .getCollection(collName)
                       .find({})
                       .readPref("secondary")
                       .batchSize(10);

    const firstDoc = cursor.next();
    assert.neq(null, firstDoc, "Should have documents in the collection");
    const cursorId = cursor.getId();
    assert.neq(0, cursorId, "Cursor should be open with more documents to fetch");

    assert.soon(
        () => st.s.adminCommand({split: ns, middle: {_id: 250}}).ok,
        "split did not complete within the timeout",
    );
    assert.soon(
        () => st.s.adminCommand({
                      moveChunk: ns,
                      find: {_id: 0},
                      to: st.shard1.shardName,
                      _waitForDelete: false,
                  })
                  .ok,
        "moveChunk did not complete within the timeout",
    );
    assert.soon(
        () => st.s.adminCommand({
                      moveChunk: ns,
                      find: {_id: 250},
                      to: st.shard1.shardName,
                      _waitForDelete: false,
                  })
                  .ok,
        "moveChunk did not complete within the timeout",
    );

    assert.soon(
        () => st.s.adminCommand({movePrimary: dbName, to: st.shard1.shardName}).ok,
        "movePrimary did not complete within the timeout",
    );

    jsTest.log("Start transition to dedicated config server");
    let res = st.s.adminCommand({transitionToDedicatedConfigServer: 1});
    assert.commandWorked(res);
    assert.eq("started", res.state, "Expected 'started' state: " + tojson(res));
    st.configRS.awaitReplication();

    jsTest.log("Wait for transition to complete");
    assert.soonNoExcept(
        () => {
            res = st.s.adminCommand({transitionToDedicatedConfigServer: 1});
            assert.commandWorked(res);
            return res.state === "completed";
        },
        "transitionToDedicatedConfigServer did not reach 'completed' state within the timeout",
        120000,
    );

    const error = assert.throws(() => {
        while (cursor.hasNext()) {
            cursor.next();
        }
    });

    assert.eq(ErrorCodes.QueryPlanKilled,
              error.code,
              "Expected QueryPlanKilled error but got: " + tojson(error));

    jsTest.log("Transition back to embedded config server");
    assert.commandWorked(st.s.adminCommand({transitionFromDedicatedConfigServer: 1}));

    assert.commandWorked(st.s.getDB(dbName).dropDatabase());
})();

st.stop();
