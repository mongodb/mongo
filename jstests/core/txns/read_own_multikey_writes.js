// Tests that multikey updates made inside a transaction are visible to that transaction's reads.
// @tags: [
//   assumes_unsharded_collection,
//   uses_transactions,
// ]
const dbName = "test";
const collName = "testReadOwnMultikeyWrites";

const session = db.getMongo().startSession({causalConsistency: false});
const sessionDb = session.getDatabase(dbName);

(function testSimpleCase() {
    // Use majority write concern to clear the drop-pending that can cause lock conflicts with
    // transactions.
    db.getSiblingDB(dbName)
        .getCollection(collName)
        .drop({writeConcern: {w: "majority"}});

    const sessionColl = sessionDb.getCollection(collName);

    assert.commandWorked(sessionDb.runCommand({create: collName}));

    assert.commandWorked(sessionColl.insert({a: 1}));
    assert.commandWorked(
        sessionDb.runCommand({
            createIndexes: collName,
            indexes: [{name: "a_1", key: {a: 1}}],
            writeConcern: {w: "majority"},
        }),
    );

    session.startTransaction();
    assert.commandWorked(sessionColl.update({}, {$set: {a: [1, 2, 3]}}));
    assert.eq(1, sessionColl.find({}, {_id: 0, a: 1}).sort({a: 1}).itcount());
    assert.commandWorked(session.commitTransaction_forTesting());

    assert.eq(
        1,
        db
            .getSiblingDB(dbName)
            .getCollection(collName)
            .find({}, {_id: 0, a: 1})
            .sort({a: 1})
            .itcount(),
    );
})();

// === Wildcard index multikey RYOW cases ===
//
// These cases verify that wildcard index multikey state written inside a multi-document
// transaction is visible to subsequent reads in the same transaction.
//
// The query pattern uses contradictory range bounds: {field: {$gt: high, $lt: low}}.
// - Multikey-aware planner: generates a union of ($gt: high) and ($lt: low) bounds and
//   post-filters; matches a document whose array contains elements satisfying each
//   predicate independently.
// - Non-multikey planner: intersects the bounds into an empty range; returns nothing.
function runWildcardCase(collName, setupFn, queryHint, query, expectedCount, message) {
    db.getSiblingDB(dbName)
        .getCollection(collName)
        .drop({writeConcern: {w: "majority"}});
    assert.commandWorked(sessionDb.runCommand({create: collName}));
    assert.commandWorked(
        sessionDb.runCommand({
            createIndexes: collName,
            indexes: [{name: "wildcard", key: {"$**": 1}}],
            writeConcern: {w: "majority"},
        }),
    );

    const wcColl = sessionDb.getCollection(collName);
    session.startTransaction();
    setupFn(wcColl);
    const results = wcColl.find(query).hint(queryHint).toArray();
    assert.eq(expectedCount, results.length, message + " got: " + tojson(results));
    assert.commandWorked(session.commitTransaction_forTesting());
}

// Sanity: a non-multikey scalar field must NOT match contradictory bounds.
runWildcardCase(
    "testReadOwnWildcardMultikeyWrites_sanity",
    (coll) => assert.commandWorked(coll.insert({a: 1})),
    {"$**": 1},
    {a: {$gt: 8, $lt: 3}},
    0,
    "wildcard sanity (non-multikey scalar):",
);

// Single multikey field.
runWildcardCase(
    "testReadOwnWildcardMultikeyWrites_single",
    (coll) => assert.commandWorked(coll.insert({a: [1, 10]})),
    {"$**": 1},
    {a: {$gt: 8, $lt: 3}},
    1,
    "wildcard RYOW single field:",
);

// Multiple multikey fields accumulate within a transaction.
(function testWildcardMultipleFields() {
    const collName = "testReadOwnWildcardMultikeyWrites_multi";
    db.getSiblingDB(dbName)
        .getCollection(collName)
        .drop({writeConcern: {w: "majority"}});
    assert.commandWorked(sessionDb.runCommand({create: collName}));
    assert.commandWorked(
        sessionDb.runCommand({
            createIndexes: collName,
            indexes: [{name: "wildcard", key: {"$**": 1}}],
            writeConcern: {w: "majority"},
        }),
    );

    const wcColl = sessionDb.getCollection(collName);
    session.startTransaction();
    assert.commandWorked(wcColl.insert({a: [1, 10]}));
    assert.commandWorked(wcColl.insert({b: [2, 20]}));

    const resultsA = wcColl
        .find({a: {$gt: 8, $lt: 3}})
        .hint({"$**": 1})
        .toArray();
    assert.eq(1, resultsA.length, "wildcard RYOW field 'a': " + tojson(resultsA));

    const resultsB = wcColl
        .find({b: {$gt: 15, $lt: 5}})
        .hint({"$**": 1})
        .toArray();
    assert.eq(1, resultsB.length, "wildcard RYOW field 'b': " + tojson(resultsB));

    assert.commandWorked(session.commitTransaction_forTesting());
})();

// Write, read, write, read — multikey state from later writes must be visible to subsequent
// reads in the same transaction.
(function testWildcardWriteReadWriteRead() {
    const collName = "testReadOwnWildcardMultikeyWrites_wrwr";
    db.getSiblingDB(dbName)
        .getCollection(collName)
        .drop({writeConcern: {w: "majority"}});
    assert.commandWorked(sessionDb.runCommand({create: collName}));
    assert.commandWorked(
        sessionDb.runCommand({
            createIndexes: collName,
            indexes: [{name: "wildcard", key: {"$**": 1}}],
            writeConcern: {w: "majority"},
        }),
    );

    const wcColl = sessionDb.getCollection(collName);
    session.startTransaction();

    assert.commandWorked(wcColl.insert({_id: 1, a: [1, 10]}));
    const r1 = wcColl
        .find({a: {$gt: 8, $lt: 3}})
        .hint({"$**": 1})
        .toArray();
    assert.eq(1, r1.length, "wildcard RYOW W/R/W/R read 1: " + tojson(r1));

    assert.commandWorked(wcColl.insert({_id: 2, b: [2, 20]}));
    const r2 = wcColl
        .find({b: {$gt: 15, $lt: 5}})
        .hint({"$**": 1})
        .toArray();
    assert.eq(1, r2.length, "wildcard RYOW W/R/W/R read 2: " + tojson(r2));

    assert.commandWorked(session.commitTransaction_forTesting());
})();

// $or across two distinct wildcard indexes within one parent transaction.
(function testWildcardOrAcrossTwoIndexes() {
    const collName = "testReadOwnWildcardMultikeyWrites_or";
    db.getSiblingDB(dbName)
        .getCollection(collName)
        .drop({writeConcern: {w: "majority"}});
    assert.commandWorked(sessionDb.runCommand({create: collName}));
    // Two wildcard indexes with disjoint projections so each branch of the $or has exactly
    // one usable wildcard index.
    assert.commandWorked(
        sessionDb.runCommand({
            createIndexes: collName,
            indexes: [
                {name: "wc_a", key: {"$**": 1}, wildcardProjection: {a: 1}},
                {name: "wc_c", key: {"$**": 1}, wildcardProjection: {c: 1}},
            ],
            writeConcern: {w: "majority"},
        }),
    );

    const wcColl = sessionDb.getCollection(collName);
    session.startTransaction();
    assert.commandWorked(wcColl.insert({_id: 1, a: [1, 10]}));
    assert.commandWorked(wcColl.insert({_id: 2, c: [2, 20]}));

    // Validate each wildcard index individually with hint by name to force IXSCAN.
    const resultsA = wcColl
        .find({a: {$gt: 8, $lt: 3}})
        .hint("wc_a")
        .toArray();
    assert.eq(1, resultsA.length, "wildcard $or per-index wc_a: " + tojson(resultsA));

    const resultsC = wcColl
        .find({c: {$gt: 15, $lt: 5}})
        .hint("wc_c")
        .toArray();
    assert.eq(1, resultsC.length, "wildcard $or per-index wc_c: " + tojson(resultsC));

    // $or query touching both wildcard indexes simultaneously.
    const results = wcColl.find({$or: [{a: {$gt: 8, $lt: 3}}, {c: {$gt: 15, $lt: 5}}]}).toArray();
    assert.eq(2, results.length, "wildcard $or across 2 wildcard indexes: " + tojson(results));

    assert.commandWorked(session.commitTransaction_forTesting());
})();
