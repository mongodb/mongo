/**
 * Test that the query engine is capable of running queries when the catalog's in memory cache of objects is destroyed during a yield.
 * This testcase serves as a regression test for SERVER-105873 which was a problem with the query optimizer stashing a raw pointer
 * to a storage-owned object which is destroyed during a yield and concurrent collMod operation. The comments inline represent the
 * state of the code before SERVER-105873 was fixed.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";

function insertDocs(coll) {
    let docs = [{a: "abc", b: "def"}];
    for (let i = 0; i < 1000; i++) {
        docs.push({
            a: 1,
            b: 1,
            c: i,
        });
    }
    assert.commandWorked(coll.insertMany(docs));
}

// Test stale pointer for partial filter expression of an index.
function testPartialIndex(conn) {
    let db = conn.getDB("test");
    const collName = jsTestName();
    const coll = db[collName];
    coll.drop();

    insertDocs(coll);

    // Ensure first branch of $or has multiple indexed plans and must run the multiplanner. This ensures
    // we yield the collection acquisition.
    assert.commandWorked(coll.createIndex({a: 1}, {partialFilterExpression: {b: 1}, name: "a_partial"}));
    assert.commandWorked(coll.createIndex({b: 1}));

    // Ensure we yield the collection acquisition.
    assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1}));

    // Configure the fail point to pause after the query planner has taken pointers to the catalog owned objects.
    let pauseAfterFillingOutIndexEntries = configureFailPoint(conn, "pauseAfterFillingOutIndexEntries");

    const awaitQuery = startParallelShell(function () {
        const coll = db[jsTestName()];
        const res = coll
            .find({
                $or: [
                    {a: 1, b: 1},
                    // Ensure one branch does not have an indexed plan. This forces the
                    // subplanner to fallback to `choosePlanWholeQuery()`.
                    {c: null},
                ],
            })
            .toArray();
        assert.eq(1001, res.length);
    }, db.getMongo().port);

    // At this point, the parallel shell's query is paused after filling out index entries in 'QueryPlannerParams' and no
    // subplanning or multiplanning has began.
    pauseAfterFillingOutIndexEntries.wait();

    // Perform a collMod which forces the catalog to destroy its in-memory cache of the 'a_partial' index.
    assert.commandWorked(db.runCommand({collMod: collName, index: {name: "a_partial", hidden: true}}));
    assert.commandWorked(db.runCommand({collMod: collName, index: {name: "a_partial", hidden: false}}));

    // Begin subplanning and multiplanning of the first branch. This results in a yield which releases the one and only reference
    // to the initial copy of the 'a_partial' IndexCatalogEntryImpl which owns the partial filter MatchExpression. On restore,
    // the catalog constructs a new copy of the 'a_partial' IndexCatalogEntryImpl. However, before SERVER-105873, the query engine
    // was saving a raw pointer to the partial filter expression owned by the first copy of the IndexCatalogEntryImpl, which at
    // this point is now destroyed. When the subplanner tries to plans the second branch, it is only able to create a collscan plan,
    // which results in falling back to `choosePlanWholeQuery()`, which tries to dereference the destroyed partial index pointer when
    // determining index eligibility.
    pauseAfterFillingOutIndexEntries.off();
    awaitQuery();
}

// Test stale pointer to wildcard projection.
// The repro is nearly identical to that of 'testPartialIndex()', so we've omitted the inline comments.
function testWildcardIndex(conn) {
    let db = conn.getDB("test");
    const collName = jsTestName();
    const coll = db[collName];
    coll.drop();
    insertDocs(coll);

    assert.commandWorked(coll.createIndex({"a.$**": 1}, {name: "a_wildcard"}));
    assert.commandWorked(coll.createIndex({b: 1}));
    assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1}));
    let pauseAfterFillingOutIndexEntries = configureFailPoint(conn, "pauseAfterFillingOutIndexEntries");
    const awaitQuery = startParallelShell(function () {
        const coll = db[jsTestName()];
        const res = coll
            .find({
                $or: [{"a.foo": 1, b: 1}, {c: null}],
            })
            .toArray();
        assert.eq(1, res.length);
    }, db.getMongo().port);

    pauseAfterFillingOutIndexEntries.wait();
    assert.commandWorked(db.runCommand({collMod: collName, index: {name: "a_wildcard", hidden: true}}));
    assert.commandWorked(db.runCommand({collMod: collName, index: {name: "a_wildcard", hidden: false}}));
    pauseAfterFillingOutIndexEntries.off();
    awaitQuery();
}

// Test stale pointer to collator.
// The repro is nearly identical to that of 'testPartialIndex()', so we've omitted the inline comments.
function testCollation(conn) {
    let db = conn.getDB("test");
    const collName = jsTestName();
    const coll = db[collName];
    coll.drop();

    // Create collection with collation
    const collation = {locale: "fr_CA", strength: 2};
    assert.commandWorked(db.createCollection(collName, {collation: collation}));
    insertDocs(coll);

    assert.commandWorked(coll.createIndex({a: 1}, {name: "a_collation", collation: collation}));
    assert.commandWorked(coll.createIndex({b: 1}, {collation: collation}));
    assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1}));
    let pauseAfterFillingOutIndexEntries = configureFailPoint(conn, "pauseAfterFillingOutIndexEntries");
    const awaitQuery = startParallelShell(function () {
        const coll = db[jsTestName()];
        const res = coll
            .find({
                // Ensure predicate has string operands so collator is consulted.
                $or: [{a: "abc", b: "def"}, {c: null}],
            })
            .collation({locale: "fr_CA", strength: 2})
            .toArray();
        assert.eq(1, res.length);
    }, db.getMongo().port);

    pauseAfterFillingOutIndexEntries.wait();
    assert.commandWorked(db.runCommand({collMod: collName, index: {name: "a_collation", hidden: true}}));
    assert.commandWorked(db.runCommand({collMod: collName, index: {name: "a_collation", hidden: false}}));
    pauseAfterFillingOutIndexEntries.off();
    awaitQuery();
}

function runTest(testFn) {
    let conn = MongoRunner.runMongod();
    try {
        testFn(conn);
    } finally {
        MongoRunner.stopMongod(conn);
    }
}

runTest(testPartialIndex);
runTest(testWildcardIndex);
runTest(testCollation);
