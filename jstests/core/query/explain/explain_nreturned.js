// Tests aimed specifically at explain() with execution stats and the reported 'nReturned' value.
// @tags: [
//   uses_explain,
//   # SERVER-29449 changed the way sharded collections report explain for queries with skip/limit.
//   requires_fcv_82,
//
//   assumes_read_concern_local,
//   requires_getmore,
// ]

const coll = db.explain_nreturned;
coll.drop();

const numDocs = 100;
const docs = [];
for (let i = 0; i < numDocs; i++) {
    docs.push({x: i});
}
assert.commandWorked(coll.insert(docs));

const predicate = {
    x: {$gt: 50},
};

function checkFind() {
    assert.eq(
        numDocs,
        coll.find().itcount(),
        "Expected find() with no predicate used with itcount() to return all docs",
    );
    assert.eq(
        49,
        coll.find(predicate).count(),
        "Expected count() with predicate to return correct result",
    );
    assert.eq(
        49,
        coll.find(predicate).itcount(),
        "Expected find() used with itcount() to return correct result",
    );
    assert.eq(
        20,
        coll.find(predicate).limit(20).itcount(),
        "Expected find() with limit to return correct result",
    );
}

function checkExplainWithExecutionStats() {
    assert.eq(
        49,
        coll.find(predicate).explain("executionStats").executionStats.nReturned,
        "nReturned on simple find() with predicate",
    );
    assert.eq(
        20,
        coll.find(predicate).limit(20).explain("executionStats").executionStats.nReturned,
        "Incorrect nReturned on find() with predicate and limit",
    );
    assert.eq(
        20,
        coll.find(predicate).limit(-20).explain("executionStats").executionStats.nReturned,
        "Incorrect nReturned on find() with predicate and negative limit",
    );
    assert.eq(
        49,
        coll.find(predicate).batchSize(20).explain("executionStats").executionStats.nReturned,
        "Incorrect nReturned on find() with predicate and non-default batch size",
    );
    // The batch size does not bound 'nReturned': explain reports the total number of documents
    // matching the query even when the batch size is much smaller than the result set.
    assert.eq(
        49,
        coll.find(predicate).batchSize(1).explain("executionStats").executionStats.nReturned,
        "Incorrect nReturned on find() with predicate and batch size smaller than the result set",
    );
    assert.eq(
        numDocs,
        coll.find().batchSize(1).explain("executionStats").executionStats.nReturned,
        "Incorrect nReturned on find() with no predicate and a small batch size",
    );
}

function runTest() {
    checkFind();
    checkExplainWithExecutionStats();
}

runTest();

assert.commandWorked(coll.createIndex({x: 1}));
runTest();
