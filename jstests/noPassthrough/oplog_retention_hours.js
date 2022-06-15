/**
 * When started with the --oplogMinRetentionHours flag, the server must enforce a minimum retention
 * time (in hours) in addition to the implicit oplogSize for the oplog.
 *
 * Only when the oplog's size has exceeded the server's --oplogSize parameter AND the timestamp
 * of the newest oplog entry in the oldest stone has fallen outside of the retention window do we
 * remove the last stone.
 *
 * This test floods the oplog collection until it reaches --oplogSize, and then checks that the
 * current size of the oplog is less than --oplogSize only after the minimum retention time has
 * passed since inserting the first set of oplog entries
 *
 * @tags: [requires_replication]
 */
(function() {
"use strict";
load("jstests/core/txns/libs/prepare_helpers.js");

const doTest = () => {
    const replSet = new ReplSetTest({
        oplogSize: PrepareHelpers.oplogSizeMB,
        // Oplog can be truncated each "sync" cycle. Increase its frequency to once per second.
        nodeOptions: {syncdelay: 1, setParameter: {logComponentVerbosity: tojson({storage: 1})}},
        nodes: 1
    });
    const oplogMinRetentionHours = 0.002777;
    const minRetention = {oplogMinRetentionHours};  // 10 seconds
    replSet.startSet(Object.assign(minRetention, PrepareHelpers.replSetStartSetOptions));
    replSet.initiate();
    const primary = replSet.getPrimary();
    let oplogEntries = primary.getDB("local").getCollection("oplog.rs");

    // ensure that oplog is not initially at capacity
    assert.lt(oplogEntries.dataSize(), PrepareHelpers.oplogSizeBytes);

    primary.startSession();

    jsTestLog("Insert documents until oplog exceeds oplogSize");
    const startTime = new Date();
    PrepareHelpers.growOplogPastMaxSize(replSet);
    // keep inserting docs until hasReplSetBeenTruncated returns true
    InsertUntilPred(replSet, didReplSetTruncate, replSet);
    const endTime = new Date();

    const kNumMSInHour = 1000 * 60 * 60;
    const truncationElapsedTime = (endTime - startTime) / kNumMSInHour;
    assert.lte(oplogMinRetentionHours, truncationElapsedTime);

    replSet.stopSet();
};

/**
 * InsertUntilPred inserts documents into a single-node replica set until the predicate argument
 * returns true.
 *
 * This helper takes in the following arguments:
 *
 *   - replSet: A single-node replica set
 *
 *   - pred: A function that returns a boolean statement. When this pred returns true, we stop
 * inserting documents
 *
 *   - args: A list of arguments that is passed into the predicate function argument as its
 * arguments
 */
const InsertUntilPred = (replSet, pred, ...args) => {
    const primary = replSet.getPrimary();
    const oplog = primary.getDB("local").oplog.rs;
    const coll = primary.getDB("insertUntilPred").growOplogPastMaxSize;
    const numNodes = replSet.nodeList().length;
    const tenKB = new Array(10 * 1024).join("a");

    print(`Oplog on ${primary} dataSize = ${oplog.dataSize()}`);
    assert.soon(
        () => {
            if (pred(...args)) {
                jsTestLog("Predicate returned true, so we're done");
                return true;
            }

            jsTestLog("Inserting a doc...");
            // insert a doc if predicate is not true
            assert.commandWorked(coll.insert({tenKB: tenKB}, {writeConcern: {w: numNodes}}));
            return false;
        },
        `timeout occurred while waiting for predicate function to return true`,
        ReplSetTest.kDefaultTimeoutMS,
        1000);
};

// checks if the oplog has been truncated
const didReplSetTruncate = replSet => {
    const oplogCol = replSet.getPrimary().getDB("local").oplog.rs;
    // The oplog milestone system allows the oplog to grow to 110% its max size.
    return oplogCol.dataSize() < 1.1 * PrepareHelpers.oplogSizeBytes;
};

doTest();
})();
