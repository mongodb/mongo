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
export function executeAggregationTestCase(collection, testCase) {
    jsTestLog(tojson(testCase));
    assert.commandWorked(collection.remove({}));

    // Insert some documents into the collection.
    assert.commandWorked(collection.insert(testCase.inputDocuments));

    // Issue an aggregate command and verify the result.
    try {
        const actualResults = collection.aggregate(testCase.pipeline).toArray();
        if (testCase.expectedResults === undefined) {
            assert(testCase.expectedErrorCode === undefined,
                   `Expected an exception with code ${testCase.expectedErrorCode}`);
        }
        assert.docEq(testCase.expectedResults, actualResults);
    } catch (error) {
        if (testCase.expectedErrorCode === undefined) {
            throw error;
        }
        assert.commandFailedWithCode(error, testCase.expectedErrorCode);
    }
}

/**
 * Parameter 'internalPipelineLengthLimit' depends on the platform and the build type.
 */
export function getExpectedPipelineLimit(database) {
    const buildInfo = assert.commandWorked(database.adminCommand("buildInfo"));
    const isDebug = buildInfo.debug;
    const isS390X =
        "buildEnvironment" in buildInfo ? buildInfo.buildEnvironment.distarch == "s390x" : false;
    return isDebug ? 200 : (isS390X ? 700 : 1000);
}