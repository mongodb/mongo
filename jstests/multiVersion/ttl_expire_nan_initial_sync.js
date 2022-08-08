/**
 * Tests that adding a node running a 5.0+ binary to an existing 4.4 cluster containing
 * a TTL index with NaN for 'expireAfterSeconds' will trigger a fassert on startup.
 *
 * @tags: [
 *     requires_replication,
 * ]
 */
(function() {
'use strict';

load('jstests/noPassthrough/libs/index_build.js');

const rst = new ReplSetTest({
    nodes: [{binVersion: 'last-lts'}],
});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
const db = primary.getDB('test');
const coll = db.t;

assert.commandWorked(coll.createIndex({t: 1}, {expireAfterSeconds: NaN}));
assert.commandWorked(coll.insert({_id: 0, t: ISODate()}));

const newNode = rst.add({
    binVersion: 'latest',
    rsConfig: {votes: 0, priority: 0},
    setParameter: {numInitialSyncAttempts: 1},
});
rst.reInitiate();

// Confirm that we are unable to use a 5.0+ server binary to join the replica set.
assert.soon(() => {
    return rawMongoProgramOutput().search(/Fatal assertion/) >= 0;
});
rst.stop(newNode, /*signal=*/undefined, {allowedExitCode: MongoRunner.EXIT_ABRUPT});

// Failed startup logs should contain details on the invalid TTL index.
assert.gte(
    rawMongoProgramOutput().search(
        /Fatal assertion.*40088.*CannotCreateIndex.*t_1.*TTL indexes cannot have NaN 'expireAfterSeconds'/),
    0);

rst.stopSet();
})();
