/**
 * Test that a replica set can process CRUD operations while undergoing shutdowns. In particular,
 * whether the fast count can remain correct while processing these requests.
 */

import {Thread} from "jstests/libs/parallelTester.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// We only care about non-retryable errors in the background CRUD threads, so retryable errors
// should be retried until they succeed.
TestData.overrideRetryAttempts = 99999;

// We configure the mongo shell to log its retry attempts so there are more diagnostics
// available in case this test ever fails.
TestData.logRetryAttempts = true;

const NUM_NODES = 3;
const NUM_ITERATIONS = 10;
const dbName = jsTestName();
const unshardedColl = "unsharded";
const unshardedNs = dbName + "." + unshardedColl;
const _id = "randomId";

/**
 * Checks that basic CRUD operations work as expected. Expects the collection to have a
 * { _id: _id } document.
 */
const checkBasicCRUD = function(withCollection, _id, isReplSetConnection) {
    const sleepMs = 1;
    const numRetries = 99999;
    const NUM_NODES = 3;

    // A CRUD operation can fail with FailedToSatisfyReadPreference during the stage
    // where the replica set primary is restarted. This is not a correctness issue and it is safe to
    // retry.
    const additionalCodesToRetry = [];
    if (isReplSetConnection) {
        additionalCodesToRetry.push(ErrorCodes.FailedToSatisfyReadPreference);
    }

    const runWithRetries = (fn) =>
        retryOnRetryableError(fn, numRetries, sleepMs, additionalCodesToRetry);

    const runFindOneWithRetries = (coll, filter) => runWithRetries(() => coll.findOne(filter));

    withCollection((coll) => {
        const doc = runFindOneWithRetries(coll, {_id: _id, y: {$exists: false}});
        assert.neq(null, doc);
    });

    withCollection((coll) => {
        assert.commandWorked(runWithRetries(() => coll.updateOne({_id: _id}, {$set: {y: 2}})));
        assert.eq(2, runFindOneWithRetries(coll, {_id: _id}).y);
    });

    withCollection((coll) => {
        assert.commandWorked(runWithRetries(() => coll.remove({_id: _id}, true /* justOne */)));
        assert.eq(null, runFindOneWithRetries(coll, {_id: _id}));
    });

    withCollection((coll) => {
        assert.commandWorked(
            runWithRetries(() => coll.insert({_id: _id}, {writeConcern: {w: NUM_NODES}})));
        assert.eq(_id, runFindOneWithRetries(coll, {_id: _id})._id);
    });
};

const checkCRUDThread = function(replSetHost, ns, _id, countdownLatch, checkBasicCRUD) {
    const [dbName, collName] = ns.split(".");
    const replSet = new Mongo(replSetHost);
    const replSetSession = replSet.startSession({retryWrites: true, causalConsistency: true});
    const replSetDb = replSetSession.getDatabase(dbName);
    const replSetColl = replSetDb[collName];

    let isReplSetConnection = false;
    const withCollection = (op) => {
        op(replSetColl);
    };

    while (countdownLatch.getCount() > 0) {
        checkBasicCRUD(withCollection, _id, isReplSetConnection);
        sleep(1);  // milliseconds.
    }
};

// Size storer-specific information, and information about rolled-back transactions (which would
// undo changes to the size storer), are both logged at verbosity level 2.
const nodeOptions = {
    setParameter: {
        logComponentVerbosity: tojson({storage: {verbosity: 2}}),
    }
};

jsTestLog("Starting up replica set");
const replSet = new ReplSetTest({nodes: NUM_NODES, nodeOptions: nodeOptions});
replSet.startSet();
replSet.initiate(null, null, {initiateWithDefaultElectionTimeout: true});
const rstPrimary = replSet.getPrimary();

// Insert the initial documents for the background CRUD threads.
assert.commandWorked(rstPrimary.getDB(dbName)[unshardedColl].insert({_id: _id}));

const crudStopLatchUnsharded = new CountDownLatch(1);
let crudThreadUnsharded = new Thread(
    checkCRUDThread, replSet.getURL(), unshardedNs, _id, crudStopLatchUnsharded, checkBasicCRUD);

jsTestLog("Starting up our CRUD thread");
crudThreadUnsharded.start();

for (let i = 0; i < NUM_ITERATIONS; i++) {
    jsTestLog("Restarting secondaries");
    const secondaries = replSet.getSecondaries();
    secondaries.forEach(secondary => {
        replSet.restart(secondary);
    });

    jsTestLog(
        "Stepping down and restarting current primary and awaiting replica set to elect new primary");
    let primary = replSet.getPrimary();
    primary.adminCommand({replSetStepDown: 600});
    replSet.restart(primary);
    replSet.awaitNodesAgreeOnPrimary();

    sleep(1000);  // Let the background CRUD operations run for a while.
}

jsTestLog("Joining background CRUD ops thread.");
crudStopLatchUnsharded.countDown();
crudThreadUnsharded.join();
// stopSet will also check for fast count consistency implicitly before shutting down the replica
// set.
replSet.stopSet();
