import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

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
            assert(
                testCase.expectedErrorCode === undefined,
                `Expected an exception with code ${testCase.expectedErrorCode}`,
            );
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
    const isS390X = "buildEnvironment" in buildInfo ? buildInfo.buildEnvironment.distarch == "s390x" : false;
    return isDebug ? 200 : isS390X ? 700 : 1000;
}

/**
 * Helper for `isSlowBuild`.
 */
function isSlowBuildInfo(buildInfo) {
    return (
        buildInfo.isDebug() ||
        !buildInfo.isOptimizationsEnabled() ||
        buildInfo.isAddressSanitizerActive() ||
        buildInfo.isLeakSanitizerActive() ||
        buildInfo.isThreadSanitizerActive() ||
        buildInfo.isUndefinedBehaviorSanitizerActive() ||
        _isSpiderMonkeyDebugEnabled()
    );
}

/**
 * For tests that run many aggregations, different build settings can affect whether we can finish
 * the test before the timeout. These settings are: whether debug is on, whether optimizations are
 * enabled, whether sanitizers are enabled, and whether spidermonkey is used.
 */
export function isSlowBuild(db) {
    if (FixtureHelpers.isMongos(db)) {
        const shardBuildInfos = FixtureHelpers.mapOnEachShardNode({
            db,
            func: (primaryDb) => primaryDb.getServerBuildInfo(),
        });
        return shardBuildInfos.some(isSlowBuildInfo);
    }
    return isSlowBuildInfo(db.getServerBuildInfo());
}
