/**
 * Executes a test case that inserts documents, issues an aggregate command on a collection
 * 'collection' and compares the results with the expected.
 *
 * testCase.inputDocuments - a document or an array of documents to insert.
 * testCase.pipeline - an aggregation pipeline to execute.
 * testCase.expectedResults - an array of documents expected to be produced by the "aggregate"
 * command.
 * testCase.expectedErrorCode - an expected error do be produced by the "aggregate" command.
 */
function executeAggregationTestCase(collection, testCase) {
    jsTestLog(tojson(testCase));
    assert.commandWorked(collection.remove({}));

    // Insert some documents into the collection.
    assert.commandWorked(collection.insert(testCase.inputDocuments));

    // Issue an aggregate command and verify the result.
    try {
        const actualResults = collection.aggregate(testCase.pipeline).toArray();
        assert(testCase.expectedErrorCode === undefined,
               `Expected an exception with code ${testCase.expectedErrorCode}`);
        assert.docEq(actualResults, testCase.expectedResults);
    } catch (error) {
        if (testCase.expectedErrorCode === undefined) {
            throw error;
        }
        assert.commandFailedWithCode(error, testCase.expectedErrorCode);
    }
}