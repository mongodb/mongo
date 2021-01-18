/**
 * Tests foreground validation's ability to fix up allowable multikey metadata problems.
 */
(function() {
load("jstests/libs/analyze_plan.js");  // For getWinningPlan to analyze explain() output.

const conn = MongoRunner.runMongod();
const dbName = jsTestName();
const collName = 'test';
const db = conn.getDB(dbName);

const assertValidate = (coll, assertFn) => {
    let res = assert.commandWorked(coll.validate());
    assertFn(res);
};

const assertIndexMultikey = (coll, hint, expectMultikey) => {
    const explain = coll.find().hint(hint).explain();
    const plan = getWinningPlan(explain.queryPlanner);
    assert.eq("FETCH", plan.stage, explain);
    assert.eq("IXSCAN", plan.inputStage.stage, explain);
    assert.eq(expectMultikey,
              plan.inputStage.isMultiKey,
              `Index multikey state "${plan.inputStage.isMultiKey}" was not "${expectMultikey}"`);
};

const runTest = (testCase) => {
    db[collName].drop();
    db.createCollection(collName);
    testCase(db[collName]);
};

// Test that validate will modify an index's multikey paths if they change.
runTest((coll) => {
    // Create an index, make the index multikey on 'a', and expect normal validation behavior.
    assert.commandWorked(coll.createIndex({a: 1, b: 1}));
    assert.commandWorked(coll.insert({_id: 1, a: [0, 1]}));
    assertValidate(coll, (res) => {
        assert(res.valid);
        assert(!res.repaired);
        assert.eq(0, res.warnings.length);
        assert.eq(0, res.errors.length);
    });
    assertIndexMultikey(coll, {a: 1, b: 1}, true);

    // Insert a document that makes the index multikey on 'b', and remove the document that
    // makes the index multikey on 'a'. Expect repair to adjust the paths.
    assert.commandWorked(coll.insert({_id: 2, b: [0, 1]}));
    assert.commandWorked(coll.remove({_id: 1}));
    assertValidate(coll, (res) => {
        assert(res.valid);
        assert(res.repaired);
        assert.eq(1, res.warnings.length);
        assert.eq(0, res.errors.length);
    });
    assertIndexMultikey(coll, {a: 1, b: 1}, true);
});

// Test that validate will unset an index's multikey flag if it no longer has multikey documents.
runTest((coll) => {
    // Create an index, make the index multikey on 'a', and expect normal validation behavior.
    assert.commandWorked(coll.createIndex({a: 1, b: 1}));
    assert.commandWorked(coll.insert({_id: 1, a: [0, 1]}));
    assertValidate(coll, (res) => {
        assert(res.valid);
        assert(!res.repaired);
        assert.eq(0, res.warnings.length);
        assert.eq(0, res.errors.length);
    });
    assertIndexMultikey(coll, {a: 1, b: 1}, true);

    // Insert a document, remove the document that makes the index multikey. Expect repair to
    // unset the multikey flag.
    assert.commandWorked(coll.insert({_id: 2, a: 1, b: 1}));
    assert.commandWorked(coll.remove({_id: 1}));
    assertValidate(coll, (res) => {
        assert(res.valid);
        assert(res.repaired);
        assert.eq(1, res.warnings.length);
        assert.eq(0, res.errors.length);
    });
    assertIndexMultikey(coll, {a: 1, b: 1}, false);
});

// Test that validate will unset the multikey flag for an index that doesn't track path-level
// metadata.
runTest((coll) => {
    // Create an index, make the index multikey on 'a', and expect normal validation behavior.
    assert.commandWorked(coll.createIndex({a: 'text'}));
    assert.commandWorked(coll.insert({_id: 1, a: 'hello world'}));
    assertValidate(coll, (res) => {
        assert(res.valid);
        assert(!res.repaired);
        assert.eq(0, res.warnings.length);
        assert.eq(0, res.errors.length);
    });
    assertIndexMultikey(coll, 'a_text', true);

    // Insert a document, remove the document that makes the index multikey. Expect repair to
    // unset the multikey flag.
    assert.commandWorked(coll.insert({_id: 2, a: 'test'}));
    assert.commandWorked(coll.remove({_id: 1}));
    assertValidate(coll, (res) => {
        assert(res.valid);
        assert(res.repaired);
        assert.eq(1, res.warnings.length);
        assert.eq(0, res.errors.length);
    });
    assertIndexMultikey(coll, 'a_text', false);
});

MongoRunner.stopMongod(conn);
})();
