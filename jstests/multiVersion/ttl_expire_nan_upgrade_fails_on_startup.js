/**
 * Tests that upgrading a server containing a TTL index with NaN for 'expireAfterSeconds'
 * will trigger a fassert on startup.
 *
 * @tags: [
 *     requires_persistence,
 *     requires_replication,
 * ]
 */
(function() {
'use strict';

load('jstests/noPassthrough/libs/index_build.js');

const rst = new ReplSetTest({
    nodes: [{binVersion: 'last-lts'}, {binVersion: 'last-lts', rsConfig: {votes: 0, priority: 0}}],
});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
const db = primary.getDB('test');
const coll = db.t;

assert.commandWorked(coll.createIndex({t: 1}, {expireAfterSeconds: NaN}));
assert.commandWorked(coll.insert({_id: 0, t: ISODate()}));

// Force checkpoint in storage engine to ensure index is part of the catalog in
// in finished state at startup.
rst.awaitReplication();
const secondary = rst.getSecondary();
assert.commandWorked(secondary.adminCommand({fsync: 1}));

// Restart the secondary with a 5.0+ binary. Since the node is not expected to complete its
// startup procedure, we wait for the fassert to show up in the logs before using ReplSetTest.stop()
// to check the process exit code.
rst.restart(
    secondary, {binVersion: 'latest', waitForConnect: false}, /*signal=*/undefined, /*wait=*/false);
assert.soon(() => {
    return rawMongoProgramOutput().search(/Fatal assertion/) >= 0;
});
rst.stop(secondary, /*signal=*/undefined, {allowedExitCode: MongoRunner.EXIT_ABORT});

// Failed startup logs should contain details on the invalid TTL index.
let logs = rawMongoProgramOutput();
assert.gte(logs.search(
               /6852200.*Found an existing TTL index with NaN 'expireAfterSeconds' in the catalog/),
           0);
assert.gte(
    logs.search(
        /6852201.*TTL indexes with NaN 'expireAfterSeconds' are not supported under FCV 4.4/),
    0);
assert.gte(logs.search(/Fatal assertion.*6852202/), 0);

rst.stopSet();
})();
