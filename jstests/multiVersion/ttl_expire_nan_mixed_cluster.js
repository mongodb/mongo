/**
 * Tests that a mixed cluster in FCV 4.4 containing a TTL index with NaN for 'expireAfterSeconds'
 * will fail to replicate the TTL index to a secondary running a 5.0+ binary.
 *
 * @tags: [
 *     requires_replication,
 * ]
 */
(function() {
'use strict';

load('jstests/noPassthrough/libs/index_build.js');

const rst = new ReplSetTest({
    nodes: [{binVersion: 'last-lts'}, {binVersion: 'latest', rsConfig: {votes: 0, priority: 0}}],
});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
const db = primary.getDB('test');
const coll = db.t;

assert.commandWorked(coll.createIndex({t: 1}, {expireAfterSeconds: NaN}));
assert.commandWorked(coll.insert({_id: 0, t: ISODate()}));

// The secondary should fail to create the TTL index with NaN for 'expireAfterSeconds' during
// oplog application shut down with a fatal assertion.
assert.soon(() => {
    return rawMongoProgramOutput().search(/Fatal assertion/) >= 0;
});
const secondary = rst.getSecondary();
rst.stop(secondary, /*signal=*/undefined, {allowedExitCode: MongoRunner.EXIT_ABRUPT});
assert.gte(
    rawMongoProgramOutput().search(
        /Fatal assertion.*34437.*CannotCreateIndex.*t_1.*TTL indexes cannot have NaN 'expireAfterSeconds'/),
    0);

rst.stopSet();
})();
