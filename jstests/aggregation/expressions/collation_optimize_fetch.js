/**
 * The combination of collation, index scan, sorting and fetching needs close consideration to
 * ensure optimal ordering of the operations. If the collation of the query is the same as the
 * collation of the index, the index can be used to satisfy group, sort and limiting before fetching
 * the data to return non ICU encoded values. The before mentioned operations can operate on ICU
 * encoded values. This testsuite analyzes the number of documents fetched from the collated
 * collection in combination with a limit operator. This optimization was added with SERVER-63132.
 *
 * @tags: [
 *   requires_fcv_71,
 *   assumes_no_implicit_collection_creation_after_drop,
 * ]
 */

const caseInsensitiveUS = {
    locale: "en",
    strength: 2
};
const caseInsensitiveDE = {
    locale: "de_AT",
    strength: 2
};
const documents = [
    {_id: 0, a: 'A', b: 'B', c: 'A', d: ['x', 'y', 'z', 'h', 't'], e: {a: 'ae', b: 'be'}},
    {_id: 1, a: 'a', b: 'B', c: 'b', d: ['x', 'y', 'z', 'h', 't'], e: {a: 'ae', b: 'be'}},
    {_id: 2, a: 'A', b: 'B', c: 'C', d: ['x', 'y', 'z', 'h', 't'], e: {a: 'ae', b: 'be'}},
    {_id: 3, a: 'a', b: 'B', c: 'D', d: ['x', 'y', 'z', 'h', 't'], e: {a: 'ae', b: 'be'}},
    {_id: 4, a: 'A', b: 'B', c: 'e', d: ['x', 'y', 'z', 'h', 't'], e: {a: 'ae', b: 'be'}},
    {_id: 5, a: 'a', b: 'b', c: 'F', d: ['x', 'y', 'z', 'h', 't'], e: {a: 'ae', b: 'be'}},
    {_id: 6, a: 'A', b: 'b', c: 'g', d: ['x', 'y', 'z', 'h', 't'], e: {a: 'ae', b: 'be'}},
    {_id: 7, a: 'a', b: 'b', c: 'H', d: ['x', 'y', 'z', 'h', 't'], e: {a: 'ae', b: 'be'}},
    {_id: 8, a: 'A', b: 'b', c: 'I', d: ['x', 'y', 'z', 'h', 't'], e: {a: 'ae', b: 'be'}},
    {_id: 9, a: 'a', b: 'b', c: 'j', d: ['x', 'y', 'z', 'h', 't'], e: {a: 'ae', b: 'be'}},
];
const indexes = [{a: 1, b: 1, c: 1}, {a: 1, d: 1}, {"e.a": 1, "e.b": 1}];

function initCollection(collectionCollation, indexCollation) {
    db.collation_optimize_fetch.drop();

    // Setup the collection.
    assert.commandWorked(db.createCollection(
        "collation_optimize_fetch", collectionCollation ? {collation: collectionCollation} : ""));

    // Setup the indexes.
    indexes.forEach(idx => (assert.commandWorked(db.collation_optimize_fetch.createIndex(
                        idx, indexCollation ? {collation: indexCollation} : ""))));

    // Insert docs.
    assert.commandWorked(db.collation_optimize_fetch.insert(documents));
}

function runTest(expectedDocumentCount) {
    // Run the tests with the provided indexes.
    assert.eq(expectedDocumentCount,
              db.collation_optimize_fetch.explain("executionStats")
                  .find({a: 'a'})
                  .sort({c: 1})
                  .limit(5)
                  .next()
                  .executionStats.totalDocsExamined);
    assert.eq(expectedDocumentCount,
              db.collation_optimize_fetch.explain("executionStats")
                  .find({a: 'a'})
                  .sort({d: 1})
                  .limit(5)
                  .next()
                  .executionStats.totalDocsExamined);
    assert.eq(expectedDocumentCount,
              db.collation_optimize_fetch.explain("executionStats")
                  .find({"e.a": 'ae'})
                  .sort({"e.b": 1})
                  .limit(5)
                  .next()
                  .executionStats.totalDocsExamined);
}

// Only 5 documents should be fetched as the sort and limit can be satisfied by the IDX.
initCollection(caseInsensitiveUS);
runTest(5);

// 10 documents need to be fetched as the IDX has a different collation than the query.
initCollection(null, caseInsensitiveUS);
runTest(10);

// Two different collations on the index and collection requires to fetch all 10 documents.
initCollection(caseInsensitiveDE, caseInsensitiveUS);
runTest(10);

// Cleanup.
db.collation_optimize_fetch.drop();
