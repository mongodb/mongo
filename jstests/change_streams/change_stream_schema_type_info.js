/**
 * A change stream must keep working when featureFlagQueryTypeInference is enabled. For a change
 * stream the main collection is the oplog rather than the user collection, so the user collection's
 * validator-derived metadata is deliberately not registered on the ExpressionContext. The stream
 * must therefore survive a validator change that would invalidate an ordinary schema-dependent
 * query.
 *
 * The behavior is not topology-dependent: the '!hasChangeStream()' guard lives in the per-mongod
 * aggregation path (OplogAggCatalogState), so the replica-set, sharded, and whole-db/whole-cluster
 * change-stream passthroughs all exercise it.
 *
 * @tags: [
 *   featureFlagQueryTypeInference,
 *   # 'constraint' validationLevel is gated by featureFlagConstraintValidationLevel (FCV 9.0).
 *   requires_fcv_90,
 *   uses_change_streams,
 *   requires_majority_read_concern,
 *   # Interleaves writes with change-stream result assertions, which no longer hold once operations
 *   # are bundled into transactions.
 *   change_stream_does_not_expect_txns,
 * ]
 */
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {ChangeStreamTest} from "jstests/libs/query/change_stream_util.js";

const testDB = db.getSiblingDB(jsTestName());
const collName = "watched";

assertDropCollection(testDB, collName);
assert.commandWorked(
    testDB.createCollection(collName, {
        validator: {a: {$type: "number"}},
        validationLevel: "constraint",
        validationAction: "error",
    }),
);
const coll = testDB[collName];
assert.commandWorked(coll.insert({_id: 0, a: 0}));

const cst = new ChangeStreamTest(testDB);
const cursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: coll});

assert.commandWorked(coll.insert({_id: 1, a: 1}));
cst.assertNextChangesEqual({
    cursor: cursor,
    expectedChanges: [
        {
            documentKey: {_id: 1},
            fullDocument: {_id: 1, a: 1},
            ns: {db: testDB.getName(), coll: collName},
            operationType: "insert",
        },
    ],
});

// Replacing the validator republishes the collection's schema metadata, which invalidates
// schema-dependent plans. Already open change streams are not affected.
assert.commandWorked(
    testDB.runCommand({
        collMod: collName,
        validator: {a: {$type: "string"}},
        validationLevel: "strict",
    }),
);

assert.commandWorked(coll.insert({_id: 2, a: "now a string"}));
cst.assertNextChangesEqual({
    cursor: cursor,
    expectedChanges: [
        {
            documentKey: {_id: 2},
            fullDocument: {_id: 2, a: "now a string"},
            ns: {db: testDB.getName(), coll: collName},
            operationType: "insert",
        },
    ],
});

cst.cleanUp();
