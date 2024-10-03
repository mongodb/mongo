/**
 * Test the use of "explain" with usage of $$SEARCH_META after a $search aggregation stage. This
 * tests all verbosities and tests when mongot returns explain only as well as explain with cursor
 * response.
 * @tags: [featureFlagSearchExplainExecutionStats]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {getAggPlanStage} from "jstests/libs/query/analyze_plan.js";
import {checkSbeRestrictedOrFullyEnabled} from "jstests/libs/query/sbe_util.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {prepareUnionWithExplain} from "jstests/with_mongot/common_utils.js";
import {
    mongotCommandForQuery,
    MongotMock,
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {
    getDefaultLastExplainContents,
    getMongotStagesAndValidateExplainExecutionStats,
    setUpMongotReturnExplain,
    setUpMongotReturnExplainAndCursor,
    setUpMongotReturnExplainAndCursorGetMore,
} from "jstests/with_mongot/mongotmock/lib/utils.js";

// Set up mongotmock and point the mongod to it.
const mongotmock = new MongotMock();
mongotmock.start();
const mongotConn = mongotmock.getConnection();

const conn = MongoRunner.runMongod({setParameter: {mongotHost: mongotConn.host}});
const dbName = jsTestName();
const db = conn.getDB(dbName);
const coll = db.search;
coll.drop();
const collName = coll.getName();
const explainObject = getDefaultLastExplainContents();

if (checkSbeRestrictedOrFullyEnabled(db) &&
    FeatureFlagUtil.isPresentAndEnabled(db.getMongo(), 'SearchInSbe')) {
    jsTestLog("Skipping the test because it only applies to $search in classic engine.");
    MongoRunner.stopMongod(conn);
    mongotmock.stop();
    quit();
}

assert.commandWorked(coll.insert({_id: 1, name: "Chekhov"}));
assert.commandWorked(coll.insert({_id: 2, name: "Dostoevsky"}));
assert.commandWorked(coll.insert({_id: 3, name: "Pushkin"}));
assert.commandWorked(coll.insert({_id: 6, name: "Tolstoy"}));
assert.commandWorked(coll.insert({_id: 8, name: "Zamatyin"}));

const collUUID = getUUIDFromListCollections(db, coll.getName());

const searchQuery = {
    query: "Chekhov",
    path: "name"
};
const explainContents = {
    profession: "writer"
};

function testUnionWith(searchCmd, verbosity) {
    {
        const history = [{
            expectedCommand: searchCmd,
            response: {
                ok: 1,
                cursor: {
                    id: NumberLong(0),
                    ns: coll.getFullName(),
                    nextBatch: [
                        {_id: 2, $searchScore: 0.654},
                        {_id: 1, $searchScore: 0.321},
                        {_id: 3, $searchScore: 0.123}
                    ]
                },
                vars: {SEARCH_META: {value: 7}},
                explain: explainContents,
            }
        }];

        // This response is for the first $search in the top level pipeline.
        assert.commandWorked(mongotConn.adminCommand(
            {setMockResponses: 1, cursorId: NumberLong(123), history: history}));
        // This response is for $search in $unionWith when the query is being executed.
        assert.commandWorked(mongotConn.adminCommand(
            {setMockResponses: 1, cursorId: NumberLong(124), history: history}));
        // $unionWith will run its subpipeline again for explain execution stats so we need to mock
        // another response.
        assert.commandWorked(mongotConn.adminCommand(
            {setMockResponses: 1, cursorId: NumberLong(125), history: history}));
    }
    const result = coll.explain(verbosity).aggregate([
        {$search: searchQuery},
        {$project: {_id: 1}},
        {
            $unionWith: {
                coll: coll.getName(),
                pipeline: [
                    {$search: searchQuery},
                    {$project: {_id: {$add: [100, "$_id"]}, meta: "$$SEARCH_META"}},
                ]
            }
        }
    ]);

    // Check stats for toplevel pipeline.
    getMongotStagesAndValidateExplainExecutionStats({
        result,
        stageType: "$_internalSearchMongotRemote",
        nReturned: NumberLong(3),
        explainObject: explainContents,
        verbosity: verbosity,
    });

    getMongotStagesAndValidateExplainExecutionStats({
        result,
        stageType: "$_internalSearchIdLookup",
        nReturned: NumberLong(3),
        verbosity: verbosity,
    });
    const projectStage = getAggPlanStage(result, "$project");
    assert.eq(NumberLong(3), projectStage["nReturned"]);

    const unionWithStage = getAggPlanStage(result, "$unionWith");
    assert.neq(unionWithStage, null, result);
    assert(unionWithStage.hasOwnProperty("nReturned"));
    assert.eq(NumberLong(6), unionWithStage["nReturned"]);

    // Check stats for $unionWith subpipeline
    const pipeline = unionWithStage["$unionWith"]["pipeline"];
    let unionSubExplain = prepareUnionWithExplain(pipeline);
    getMongotStagesAndValidateExplainExecutionStats({
        result: unionSubExplain,
        stageType: "$_internalSearchMongotRemote",
        verbosity,
        nReturned: NumberLong(3),
        explainObject: explainContents,
    });
    getMongotStagesAndValidateExplainExecutionStats({
        result: unionSubExplain,
        stageType: "$_internalSearchIdLookup",
        verbosity,
        nReturned: NumberLong(3),
    });

    const unionWithProjectStage = pipeline[2];
    assert.neq(unionWithProjectStage, null, unionWithStage);
    assert(unionWithProjectStage["$project"], unionWithProjectStage);
    assert.eq(NumberLong(3), unionWithProjectStage["nReturned"]);
}

function runExplainTest(verbosity) {
    const searchCmd = mongotCommandForQuery({
        query: searchQuery,
        collName: collName,
        db: dbName,
        collectionUUID: collUUID,
        explainVerbosity: {verbosity}
    });
    const pipeline = [
        {$search: searchQuery},
        {$project: {_id: 1, meta: "$$SEARCH_META"}},
    ];
    const vars = {SEARCH_META: {value: 42}};
    {
        // TODO SERVER-91594: setUpMongotReturnExplain() should only be run for 'queryPlanner'
        // verbosity when mongot always returns a cursor for execution stats. Remove the extra
        // setUpMongotReturnExplain() for non "queryPlanner" verbosities.
        setUpMongotReturnExplain({
            mongotMock: mongotmock,
            searchCmd,
        });
        if (verbosity != "queryPlanner") {
            // When querying an older version of mongot for explain, the query is sent twice.
            // This uses a different cursorId than the default one for setUpMongotReturnExplain() so
            // the mock will return the response correctly.
            setUpMongotReturnExplain({
                mongotMock: mongotmock,
                searchCmd,
                cursorId: NumberLong(124),
            });
        }
        const result = coll.explain(verbosity).aggregate(pipeline);
        getMongotStagesAndValidateExplainExecutionStats({
            result,
            stageType: "$_internalSearchMongotRemote",
            verbosity,
            nReturned: NumberLong(0),
            explainObject
        });
        getMongotStagesAndValidateExplainExecutionStats({
            result,
            stageType: "$_internalSearchIdLookup",
            verbosity,
            nReturned: NumberLong(0),
        });
    }
    if (verbosity != "queryPlanner") {
        {
            setUpMongotReturnExplainAndCursor({
                mongotMock: mongotmock,
                coll,
                searchCmd,
                vars,
                nextBatch: [
                    {_id: 2, $searchScore: 0.654},
                    {_id: 1, $searchScore: 0.321},
                    {_id: 3, $searchScore: 0.123}
                ],
            });
            const result = coll.explain(verbosity).aggregate(pipeline);
            getMongotStagesAndValidateExplainExecutionStats({
                result,
                stageType: "$_internalSearchMongotRemote",
                verbosity,
                nReturned: NumberLong(3),
                explainObject
            });
            getMongotStagesAndValidateExplainExecutionStats({
                result,
                stageType: "$_internalSearchIdLookup",
                verbosity,
                nReturned: NumberLong(3),
            });
            const projectStage = getAggPlanStage(result, "$project");
            assert.eq(NumberLong(3), projectStage["nReturned"]);
        }

        {
            setUpMongotReturnExplainAndCursorGetMore({
                mongotMock: mongotmock,
                coll,
                searchCmd,
                batchList: [
                    [{_id: 0, $searchScore: 1.234}, {_id: 5, $searchScore: 1.21}],
                    [{_id: 2, $searchScore: 1.1}, {_id: 6, $searchScore: 0.8}],
                    [{_id: 8, $searchScore: 0.2}]
                ],
                vars,
            });
            const result = coll.explain(verbosity).aggregate(pipeline, {cursor: {batchSize: 2}});
            getMongotStagesAndValidateExplainExecutionStats({
                result,
                stageType: "$_internalSearchMongotRemote",
                verbosity,
                nReturned: NumberLong(5),
                explainObject
            });
            getMongotStagesAndValidateExplainExecutionStats({
                result,
                stageType: "$_internalSearchIdLookup",
                verbosity,
                nReturned: NumberLong(3),
            });
            const projectStage = getAggPlanStage(result, "$project");
            assert.eq(NumberLong(3), projectStage["nReturned"]);
        }

        testUnionWith(searchCmd, verbosity);
    }
}

runExplainTest("queryPlanner");
runExplainTest("executionStats");
runExplainTest("allPlansExecution");

MongoRunner.stopMongod(conn);
mongotmock.stop();
