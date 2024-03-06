/**
 * Ensures that concurrent deletions of unindexed keys on a unique partial index do not write
 * conflict with each other.
 *
 * @tags: [
 *   # We cannot create a unique index other than the shard key on sharded clusters.
 *   assumes_against_mongod_not_mongos,
 *   uses_transactions
 * ]
 */

const dbName = "test";
const collName = "concurrent_unique_partial_index";

const testDB = db.getSiblingDB(dbName);
const coll = testDB[collName];
assert.commandWorked(
    coll.createIndex({a: 1}, {unique: true, partialFilterExpression: {active: true}}));

const sessionA = db.getMongo().startSession({causalConsistency: false});
const dbSessionA = sessionA.getDatabase(dbName);
const collSessionA = dbSessionA.getCollection(collName);

const sessionB = db.getMongo().startSession({causalConsistency: false});
const dbSessionB = sessionB.getDatabase(dbName);
const collSessionB = dbSessionB.getCollection(collName);

// Each transaction inserts the same unique key that will not be indexed because the document does
// not match the partial filter expression.
sessionA.startTransaction();
assert.commandWorked(collSessionA.insert({_id: 0, a: "unique", active: false}));

sessionB.startTransaction();
assert.commandWorked(collSessionB.insert({_id: 1, a: "unique", active: false}));

// Each transaction removes the recently inserted document and ensures that no write conflict is
// triggered.
assert.commandWorked(collSessionB.remove({_id: 1}));
sessionB.commitTransaction();

assert.commandWorked(collSessionA.remove({_id: 0}));
sessionA.commitTransaction();
