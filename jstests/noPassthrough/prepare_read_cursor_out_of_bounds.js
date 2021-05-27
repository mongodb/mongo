/**
 * Reproduces a bug where searching for a key returns an adjacent key in a recently-committed
 * prepared transaction. See SERVER-56839.
 *
 * Create an index with a single key, "a". Insert a new key for "b" in a prepared transaction. This
 * creates a prepared, but uncommitted index entry before the key we want to search for, "c", which
 * doesn't exist. We will query (search_near internally) for "c" and the cursor will initially land
 * on "a". This is less than they key were searching for, so the cursor is advanced to the next key,
 * expecting to land on something greater than or equal to "c". Before this happens, the prepared
 * transaction commits, making "b" visible. Ensure that the cursor does not return "b" even though
 * we queried for "c".
 *
 * @tags: [
 *   requires_replication,
 *   uses_transactions,
 * ]
 */
(function() {
"use strict";
load("jstests/core/txns/libs/prepare_helpers.js");
load("jstests/libs/fail_point_util.js");
load('jstests/libs/parallel_shell_helpers.js');

const replTest = new ReplSetTest({nodes: 1});
replTest.startSet();
replTest.initiate();

const primary = replTest.getPrimary();
const dbName = 'test';
const collName = 'coll';

const db = primary.getDB(dbName);
assert.commandWorked(db[collName].createIndex({x: 1}));
assert.commandWorked(db[collName].insert({x: 'a'}));

const session = primary.startSession({causalConsistency: false});
const sessionDB = session.getDatabase(dbName);
const sessionColl = sessionDB.getCollection(collName);
session.startTransaction();
sessionColl.insert({x: 'b'});
const prepareTimestamp = PrepareHelpers.prepareTransaction(session);

const failpoint = configureFailPoint(primary, "WTIndexPauseAfterSearchNear", {indexName: 'x_1'});

// After the query on 'c' starts, we commit the transaction and advance the cursor. Expect that this
// finds nothing.
const awaitShell = startParallelShell(function() {
    assert.eq(null, db.coll.findOne({x: 'c'}));
}, primary.port);

failpoint.wait();
assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));
failpoint.off();
awaitShell();

replTest.stopSet();
})();
