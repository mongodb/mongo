/**
 * Tests that the batchSize field is sent to mongot correctly on GetMore requests.
 * @tags: [requires_fcv_81]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {getAggPlanStage} from "jstests/libs/query/analyze_plan.js";
import {checkSbeRestrictedOrFullyEnabled} from "jstests/libs/query/sbe_util.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    mockAllRequestsWithBatchSizes,
    mongotCommandForQuery,
    MongotMock,
    mongotResponseForBatch
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

const dbName = "test";
const collName = jsTestName();

// Start mock mongot.
const mongotMock = new MongotMock();
mongotMock.start();
const mockConn = mongotMock.getConnection();

// Start mongod.
const conn = MongoRunner.runMongod({setParameter: {mongotHost: mockConn.host}});
let db = conn.getDB(dbName);
let coll = db.getCollection(collName);
coll.drop();

if (checkSbeRestrictedOrFullyEnabled(db) &&
    FeatureFlagUtil.isPresentAndEnabled(db.getMongo(), 'SearchInSbe')) {
    jsTestLog("Skipping the test because it only applies to $search in classic engine.");
    MongoRunner.stopMongod(conn);
    mongotMock.stop();
    quit();
}

const mongotQuery = {
    query: "foo",
    path: "bar",
    returnStoredSource: true
};
const numDocs = 10000;
let docs = [];
let mongotDocs = [];
let searchScore = 0.60000;
for (let i = 0; i < numDocs; i++) {
    docs.push({_id: i, a: i % 1000, bar: "fooey"});
    mongotDocs.push({_id: i, $searchScore: searchScore, a: i % 1000, bar: "fooey"});

    // The documents with lower _id will have a higher search score.
    searchScore = searchScore - 0.00005;
}

assert.commandWorked(coll.insertMany(docs));
const collUUID = getUUIDFromListCollections(db, coll.getName());

// The batchSizeGrowthFactor is customizable as a cluster parameter. We'll assert that it's
// properly configurable and that the new growth factors are applied correctly.
function assertGrowthFactorSetAsExpected(expectedGrowthFactor) {
    assert.eq(expectedGrowthFactor,
              assert.commandWorked(db.adminCommand({getClusterParameter: "internalSearchOptions"}))
                  .clusterParameters[0]
                  .batchSizeGrowthFactor);
}

// Tests a pipeline that will exhaust all mongot results because of a blocking $group stage.
const searchGroupPipeline = [{$search: mongotQuery}, {$group: {_id: "$bar", avg: {$avg: "$a"}}}];
function testSearchGroupPipeline() {
    const res = coll.aggregate(searchGroupPipeline).toArray();
    assert.eq(res.length, 1);
    assert.eq(res[0], {_id: "fooey", avg: 499.5});
}

function testSearchGroupPipelineExplain(batchSizeArray) {
    testBatchSizeExplain(batchSizeArray, searchGroupPipeline);
}

// Tests a pipeline that will exhaust many but not all mongot results (at least up to _id=4000, but
// likely further with pre-fetching) due to a $limit preceded by a highly selective $match.
const searchMatchSmallLimitPipeline = [{$search: mongotQuery}, {$match: {a: 0}}, {$limit: 5}];
function testSearchMatchSmallLimitPipeline() {
    const res = coll.aggregate(searchMatchSmallLimitPipeline).toArray();
    assert.eq(res.length, 5);
    assert.eq(res, [
        {_id: 0, a: 0, bar: "fooey"},
        {_id: 1000, a: 0, bar: "fooey"},
        {_id: 2000, a: 0, bar: "fooey"},
        {_id: 3000, a: 0, bar: "fooey"},
        {_id: 4000, a: 0, bar: "fooey"}
    ]);
}
function testSearchMatchSmallLimitPipelineExplain(batchSizeArray) {
    testBatchSizeExplain(batchSizeArray, searchMatchSmallLimitPipeline);
}

// Tests a pipeline that will exhaust all mongot results since the $match is so selective that the
// higher $limit will not be reached.
const searchMatchLargeLimitPipeline =
    [{$search: mongotQuery}, {$match: {a: 0}}, {$limit: 250}, {$project: {_id: 1, bar: 1}}];
function testSearchMatchLargeLimitPipeline() {
    const res = coll.aggregate(searchMatchLargeLimitPipeline).toArray();
    assert.eq(res.length, 10);
    assert.eq(res, [
        {_id: 0, bar: "fooey"},
        {_id: 1000, bar: "fooey"},
        {_id: 2000, bar: "fooey"},
        {_id: 3000, bar: "fooey"},
        {_id: 4000, bar: "fooey"},
        {_id: 5000, bar: "fooey"},
        {_id: 6000, bar: "fooey"},
        {_id: 7000, bar: "fooey"},
        {_id: 8000, bar: "fooey"},
        {_id: 9000, bar: "fooey"}
    ]);
}

function testSearchMatchLargeLimitPipelineExplain(batchSizeArray) {
    testBatchSizeExplain(batchSizeArray, searchMatchLargeLimitPipeline);
}

function testBatchSizeExplain(batchSizeArray, pipeline) {
    if (FeatureFlagUtil.isEnabled(db.getMongo(), 'SearchExplainExecutionStats')) {
        mockRequests(batchSizeArray, {verbosity: "executionStats"});
        const explainResult = coll.explain("executionStats").aggregate(pipeline);
        const searchStage = getAggPlanStage(explainResult, "$_internalSearchMongotRemote");
        const stage = searchStage["$_internalSearchMongotRemote"];
        assert(stage.hasOwnProperty("internalMongotBatchSizeHistory"), stage);
        // Explain output includes value x as NumberLong(x);
        var numberLongBatchSizeArray = batchSizeArray.map(function(batchSize) {
            return NumberLong(batchSize);
        });
        assert.eq(numberLongBatchSizeArray, stage["internalMongotBatchSizeHistory"]);
    }
}

function mockRequests(expectedBatchSizes, explainVerbosity = null) {
    mockAllRequestsWithBatchSizes({
        query: mongotQuery,
        collName,
        dbName,
        collectionUUID: collUUID,
        documents: mongotDocs,
        expectedBatchSizes,
        cursorId: NumberLong(99),
        mongotMockConn: mongotMock,
        explainVerbosity,
    });
}

// Test first that the pipelines calculate each batchSize using the default growth factor of 1.50.
{
    // Assert the batchSizeGrowthFactor is set to default value of 1.50 at startup.
    assertGrowthFactorSetAsExpected(1.50);

    const searchGroupBatchList = [101, 152, 228, 342, 513, 770, 1155, 1733, 2600, 3900];
    mockRequests(searchGroupBatchList);
    testSearchGroupPipeline();
    testSearchGroupPipelineExplain(searchGroupBatchList);

    const searchMatchSmallLimitBatchList = [101, 152, 228, 342, 513, 770, 1155, 1733, 2600];
    mockRequests(searchMatchSmallLimitBatchList);
    testSearchMatchSmallLimitPipeline();
    testSearchMatchSmallLimitPipelineExplain(searchMatchSmallLimitBatchList);

    const searchMatchLargeLimitBatchList = [250, 375, 563, 845, 1268, 1902, 2853, 4280];
    mockRequests(searchMatchLargeLimitBatchList);
    testSearchMatchLargeLimitPipeline();
    testSearchMatchLargeLimitPipelineExplain(searchMatchLargeLimitBatchList);
}

// Confirm that the batchSizeGrowthFactor can be configured to 2.0 and that the same pipelines will
// calculate each batchSize using that growth factor.
{
    assert.commandWorked(db.adminCommand(
        {setClusterParameter: {internalSearchOptions: {batchSizeGrowthFactor: 2.0}}}));
    assertGrowthFactorSetAsExpected(2.0);

    const searchGroupBatchList = [101, 202, 404, 808, 1616, 3232, 6464];
    mockRequests(searchGroupBatchList);
    testSearchGroupPipeline();
    testSearchGroupPipelineExplain(searchGroupBatchList);

    // batchSize starts at 101 since {$limit: 5} is less than the default batchSize of 101 and the
    // presence of the $match means the limit is not cleanly extractable. This query doesn't exhaust
    // all results since the limit will be satisfied once document with _id=4000 is retrieved.
    const searchMatchSmallLimitBatchList = [101, 202, 404, 808, 1616, 3232, 6464];
    mockRequests(searchMatchSmallLimitBatchList);
    testSearchMatchSmallLimitPipeline();
    testSearchMatchSmallLimitPipelineExplain(searchMatchSmallLimitBatchList);

    // batchSize starts at 250 due to {$limit: 250}.
    const searchMatchLargeLimitBatchList = [250, 500, 1000, 2000, 4000, 8000];
    mockRequests(searchMatchLargeLimitBatchList);
    testSearchMatchLargeLimitPipeline();
    testSearchMatchLargeLimitPipelineExplain(searchMatchLargeLimitBatchList);
}

// Confirm that the batchSizeGrowthFactor can be configured to 2.8 and that the same pipelines will
// calculate each batchSize using that growth factor.
{
    assert.commandWorked(db.adminCommand(
        {setClusterParameter: {internalSearchOptions: {batchSizeGrowthFactor: 2.8}}}));
    assertGrowthFactorSetAsExpected(2.8);

    const searchGroupBatchList = [101, 283, 793, 2221, 6219, 17414];
    mockRequests(searchGroupBatchList);
    testSearchGroupPipeline();
    testSearchGroupPipelineExplain(searchGroupBatchList);

    const searchMatchSmallLimitBatchList = [101, 283, 793, 2221, 6219, 17414];
    mockRequests(searchMatchSmallLimitBatchList);
    testSearchMatchSmallLimitPipeline();
    testSearchMatchSmallLimitPipelineExplain(searchMatchSmallLimitBatchList);

    const searchMatchLargeLimitBatchList = [250, 700, 1960, 5488, 15367];
    mockRequests(searchMatchLargeLimitBatchList);
    testSearchMatchLargeLimitPipeline();
    testSearchMatchLargeLimitPipelineExplain(searchMatchLargeLimitBatchList);
}

// Confirm that the batchSizeGrowthFactor can be configured to 1 and that the same pipelines will
// calculate each batchSize using that growth factor.
{
    assert.commandWorked(db.adminCommand(
        {setClusterParameter: {internalSearchOptions: {batchSizeGrowthFactor: 1}}}));
    assertGrowthFactorSetAsExpected(1.00);

    // We need 100 batches with size 101 to exhaust all mongot results.
    const searchGroupBatchList = Array(100).fill(101);
    mockRequests(searchGroupBatchList);
    testSearchGroupPipeline();
    testSearchGroupPipelineExplain(searchGroupBatchList);

    const searchMatchSmallLimitBatchList = Array(41).fill(101);
    mockRequests(searchMatchSmallLimitBatchList);
    testSearchMatchSmallLimitPipeline();
    testSearchMatchSmallLimitPipelineExplain(searchMatchSmallLimitBatchList);

    const searchMatchLargeLimitBatchList = Array(41).fill(250);
    mockRequests(searchMatchLargeLimitBatchList);
    testSearchMatchLargeLimitPipeline();
    testSearchMatchLargeLimitPipelineExplain(searchMatchLargeLimitBatchList);
}

MongoRunner.stopMongod(conn);
mongotMock.stop();
