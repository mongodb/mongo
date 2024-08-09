/**
 * Verify that the write operation succeeds despite the database being dropped after the implicit
 * creation and before the operation is targeted by the router.
 *
 * @tags: [
 *   # TODO (SERVER-84043): Requires the mongos to define the fail point. Enable multiversion
 *   # once 8.0 becomes last LTS.
 *   multiversion_incompatible,
 *   # The createDatabase command inside of the write might fail with FailedToSatisfyReadPreference
 *   # if it does not find a primary. This is not a retriable error, and is correct, but
 *   # incompatible with this test.
 *   does_not_support_stepdowns,
 * ]
 */

load('jstests/libs/fail_point_util.js');
load('jstests/libs/parallelTester.js');

const dbName = 'test';
const collNS = dbName + '.foo';

const st = new ShardingTest({mongos: 1, shards: 1, config: 1});

// Pause the write operation after creating the database but before the operation is actually
// targeted by the router.
let failPoint = configureFailPoint(st.s, 'waitForDatabaseToBeDropped');

let insertThread = new Thread((mongosConnString, collNS) => {
    let mongos = new Mongo(mongosConnString);
    assert.commandWorked(mongos.getCollection(collNS).insert({}));
}, st.s0.host, collNS);

// Perform a write operation, the database is implicitly created then the operation is paused.
insertThread.start();
failPoint.wait();

// Before the router targets the write operation, the database is dropped.
assert.commandWorked(st.s0.getDB(dbName).runCommand({dropDatabase: 1}));
failPoint.off();

// The first targeting fails, then the database is recreated and the new targeting succeeds.
insertThread.join();

st.stop();
