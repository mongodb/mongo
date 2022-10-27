/**
 * Tests that data is consistent after a failover, clean, or unclean shutdown occurs in the middle
 * of a batched delete.
 * @tags: [
 *  # TODO (SERVER-55909): make WUOW 'groupOplogEntries' the only mode of operation.
 *  does_not_support_transactions,
 *  exclude_from_large_txns,
 *  requires_replication,
 *  requires_fcv_61
 * ]
 */
(function() {
"use strict";

load("jstests/libs/collection_drop_recreate.js");  // For 'assertDropCollection()'
load("jstests/libs/fail_point_util.js");           // For 'configureFailPoint()'
load("jstests/libs/parallelTester.js");            // For 'startParallelShell()'
load('jstests/libs/parallel_shell_helpers.js');    // For 'funWithArgs()'

Random.setRandomSeed();

function stepUpNewPrimary(replSet) {
    jsTestLog(`Stepping up new primary`);

    let originalPrimary = replSet.getPrimary();
    let secondaries = replSet.getSecondaries();

    // The test relies on the batched delete failing to commit on the primary. The original primary
    // must step down to ensure the batched delete cannot succeed. Additionally, if the original
    // primary doesn't step down, the 'batchedDeleteStageSleepAfterNDocuments' failpoint will never
    // get cancelled and the test will hang.
    assert.soon(() => {
        const newPrimaryIdx = Random.randInt(secondaries.length);
        const newPrimary = secondaries[newPrimaryIdx];

        let res;
        try {
            res = newPrimary.adminCommand({replSetStepUp: 1});
        } catch (e) {
            if (!isNetworkError(e)) {
                throw e;
            }

            jsTest.log(
                `Got a network error ${tojson(e)} while` +
                ` attempting to step up secondary ${newPrimary.host}. This is likely due to` +
                ` the secondary previously having transitioned through ROLLBACK and closing` +
                ` its user connections. Will retry stepping up the same secondary again`);
            res = newPrimary.adminCommand({replSetStepUp: 1});
        }

        if (res.ok === 1) {
            replSet.awaitNodesAgreeOnPrimary();
            assert.eq(newPrimary, replSet.getPrimary());
            return true;
        }

        jsTest.log(`Failed to step up secondary ${newPrimary.host} and` +
                   ` got error ${tojson(res)}. Will retry until one of the secondaries step up`);
        return false;
    });
}

function killAndRestartPrimaryOnShard(replSet) {
    jsTestLog(`Killing and restarting primary`);
    const originalPrimaryConn = replSet.getPrimary();

    const SIGKILL = 9;
    const opts = {allowedExitCode: MongoRunner.EXIT_SIGKILL};
    replSet.restart(originalPrimaryConn, opts, SIGKILL);
    replSet.awaitNodesAgreeOnPrimary();
}

function shutdownAndRestartPrimary(replSet) {
    jsTestLog(`Shutting down and restarting primary`);

    const originalPrimaryConn = replSet.getPrimary();

    const SIGTERM = 15;
    replSet.restart(originalPrimaryConn, {}, SIGTERM);
    replSet.awaitNodesAgreeOnPrimary();
}

function getCollection(rst, dbName, collName) {
    return rst.getPrimary().getDB(dbName).getCollection(collName);
}

const rst = new ReplSetTest({
    nodes: 3,
});
const nodes = rst.startSet();
rst.initiate();
rst.awaitNodesAgreeOnPrimary();

const dbName = "test";
const collName = "collHangBatchedDelete";

function runTest(failoverFn, clustered, expectNetworkErrorOnDelete) {
    let primary = rst.getPrimary();
    let testDB = primary.getDB(dbName);
    let coll = testDB.getCollection(collName);

    const ns = coll.getFullName();

    assertDropCollection(testDB, collName);

    if (clustered) {
        assert.commandWorked(
            testDB.createCollection(collName, {clusteredIndex: {key: {_id: 1}, unique: true}}));
    }

    const collCount = 5032;  // Intentionally not a multiple of the default batch size.

    const docs = [...Array(collCount).keys()].map(x => ({_id: x, a: "a".repeat(1024), b: 2 * x}));

    assert.commandWorked(coll.insertMany(docs, {ordered: false}));

    // Create secondary indexes.
    assert.commandWorked(coll.createIndex({a: 1, b: -1}));
    assert.commandWorked(coll.createIndex({_id: 1, a: 1}));
    assert.commandWorked(coll.createIndex({b: 1}));

    const hangAfterApproxNDocs = Random.randInt(collCount);
    jsTestLog(`About to hang batched delete after evaluating approximately ${
        hangAfterApproxNDocs} documents`);

    // When the delete fails, the failpoint will automatically unpause. If the connection is killed,
    // it is unsafe to try and disable the failpoint tied to testDB's original connection.
    const fp = configureFailPoint(
        testDB,
        "batchedDeleteStageSleepAfterNDocuments",
        {nDocs: hangAfterApproxNDocs, ns: coll.getFullName(), sleepMs: 60 * 60 * 1000});

    const awaitDeleteFails = startParallelShell(
        funWithArgs((dbName, collName, expectNetworkErrorOnDelete) => {
            const testDB = db.getSiblingDB(dbName);
            const coll = testDB.getCollection(collName);
            try {
                assert.commandFailed(testDB.runCommand(
                    {delete: collName, deletes: [{q: {_id: {$gte: 0}}, limit: 0}]}));
            } catch (e) {
                if (!isNetworkError(e) || !expectNetworkErrorOnDelete) {
                    // On unclean shutdown, expect a network error.
                    throw e;
                }
            }
        }, dbName, collName, expectNetworkErrorOnDelete), primary.port);

    fp.wait();

    failoverFn(rst);
    awaitDeleteFails();

    rst.awaitReplication();

    coll = getCollection(rst, dbName, collName);
    const docsDeleted = collCount - coll.count();
    assert.lte(docsDeleted, hangAfterApproxNDocs);

    rst.checkReplicatedDataHashes();
}

function runTestSet(clustered) {
    jsTestLog(`Running set of tests - collections clustered? ${clustered ? "true" : "false"}`);
    runTest(stepUpNewPrimary, clustered, false /* expectNetworkErrorOnDelete */);
    runTest(killAndRestartPrimaryOnShard, clustered, true /* expectNetworkErrorOnDelete */);
    runTest(shutdownAndRestartPrimary, clustered, false /* expectNetworkErrorOnDelete */);
}
runTestSet(false);
runTestSet(true);

rst.stopSet();
})();
