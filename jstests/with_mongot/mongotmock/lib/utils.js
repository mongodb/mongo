/**
 * Utility functions for mongot tests.
 */

import {getAggPlanStage} from "jstests/libs/analyze_plan.js";
import {mongotResponseForBatch} from "jstests/with_mongot//mongotmock/lib/mongotmock.js";
/**
 * @param {Object} expectedCommand The expected mongot $search or $vectorSearch command.
 * @param {DBCollection} coll
 * @param {Mongo} mongotConn
 * @param {NumberLong} cursorId
 * @param {String} searchScoreKey The key could be either '$searchScore' or '$vectorSearchScore'.
 * @returns {Array{Object}} Returns expected results.
 */
export function prepMongotResponse(expectedCommand,
                                   coll,
                                   mongotConn,
                                   cursorId = NumberLong(123),
                                   searchScoreKey = '$vectorSearchScore') {
    const history = [
        {
            expectedCommand,
            response: {
                cursor: {
                    id: cursorId,
                    ns: coll.getFullName(),
                    nextBatch: [
                        {_id: 1, ...Object.fromEntries([[searchScoreKey, 0.789]])},
                        {_id: 2, ...Object.fromEntries([[searchScoreKey, 0.654]])},
                        {_id: 4, ...Object.fromEntries([[searchScoreKey, 0.345]])},
                    ]
                },
                ok: 1
            }
        },
        {
            expectedCommand: {getMore: cursorId, collection: coll.getName()},
            response: {
                cursor: {
                    id: cursorId,
                    ns: coll.getFullName(),
                    nextBatch: [{_id: 11, ...Object.fromEntries([[searchScoreKey, 0.321]])}]
                },
                ok: 1
            }
        },
        {
            expectedCommand: {getMore: cursorId, collection: coll.getName()},
            response: {
                ok: 1,
                cursor: {
                    id: NumberLong(0),
                    ns: coll.getFullName(),
                    nextBatch: [{_id: 14, ...Object.fromEntries([[searchScoreKey, 0.123]])}]
                },
            }
        },
    ];

    assert.commandWorked(
        mongotConn.adminCommand({setMockResponses: 1, cursorId: cursorId, history: history}));

    return [
        {_id: 1, shardKey: 0, x: "ow"},
        {_id: 2, shardKey: 0, x: "now", y: "lorem"},
        {_id: 4, shardKey: 0, x: "cow", y: "lorem ipsum"},
        {_id: 11, shardKey: 100, x: "brown", y: "ipsum"},
        {_id: 14, shardKey: 100, x: "cow", y: "lorem ipsum"},
    ];
}

/**
 * @param {Mongo} conn
 * @param {String} dbName
 * @param {String} collName
 * @param {Boolean} shouldShard
 */
export function prepCollection(conn, dbName, collName, shouldShard = false) {
    const db = conn.getDB(dbName);
    const coll = db.getCollection(collName);
    coll.drop();

    if (shouldShard) {
        // Create and shard the collection so the commands can succeed.
        assert.commandWorked(db.createCollection(collName));
        assert.commandWorked(conn.adminCommand({enableSharding: dbName}));
        assert.commandWorked(
            conn.adminCommand({shardCollection: coll.getFullName(), key: {shardKey: 1}}));
    }

    assert.commandWorked(coll.insert([
        // Documents that end up on shard0.
        {_id: 1, shardKey: 0, x: "ow"},
        {_id: 2, shardKey: 0, x: "now", y: "lorem"},
        {_id: 3, shardKey: 0, x: "brown", y: "ipsum"},
        {_id: 4, shardKey: 0, x: "cow", y: "lorem ipsum"},
        // Documents that end up on shard1.
        {_id: 11, shardKey: 100, x: "brown", y: "ipsum"},
        {_id: 12, shardKey: 100, x: "cow", y: "lorem ipsum"},
        {_id: 13, shardKey: 100, x: "brown", y: "ipsum"},
        {_id: 14, shardKey: 100, x: "cow", y: "lorem ipsum"},
    ]));
}

// Explain object used for all explains except the last explain object.
const defaultExplainContents = {
    profession: "writer"
};

// Explain object used for the last explain returned.
const defaultLastExplainContents = {
    profession: "teacher"
};

export function getDefaultLastExplainContents() {
    return defaultLastExplainContents;
}

/**
 * Mocks mongot returning an explain object only to a search query. Checks stats and returns result
 * of executing pipeline with explain verbosity set to 'verbosity'.
 *
 * @param {MongotMock} mongotMock fixture
 * @param {Object} searchCmd the search query to send to mongot
 * @param {NumberLong} cursorId
 * @param {Object} explainContents optional explain object for mongot to return
 *
 */
export function setUpMongotReturnExplain({
    mongotMock,
    searchCmd,
    cursorId = NumberLong(123),
    explainContents = defaultLastExplainContents
}) {
    {
        const history = [{expectedCommand: searchCmd, response: {explain: explainContents, ok: 1}}];
        mongotMock.setMockResponses(history, cursorId);
    }
}

/**
 * Mocks mongot returning an explain object and exhausted cursor to a search query. Checks stats and
 * returns result of executing pipeline with explain verbosity set to 'verbosity'. Uses provided
 * batch for cursor response.
 *
 * @param {MongotMock} mongotMock fixture
 * @param {DBCollection} coll
 * @param {Object} searchCmd the search query to send to mongot
 * @param {Array} nextBatch batch of results that mongot should return
 * @param {NumberLong} cursorId optional
 * @param {Object} vars optional - vars that mongot should return
 * @param {Object} explainContents optional - explain object that mongot should return
 */
export function setUpMongotReturnExplainAndCursor({
    mongotMock,
    coll,
    searchCmd,
    nextBatch,
    cursorId = NumberLong(123),
    vars = null,
    explainContents = defaultLastExplainContents,
}) {
    {
        const history = [{
            expectedCommand: searchCmd,
            response: mongotResponseForBatch(
                nextBatch, NumberLong(0), coll.getFullName(), 1, explainContents, vars)
        }];
        mongotMock.setMockResponses(history, cursorId);
    }
}

/**
 * Mocks mongot returning an explain object and cursor with getMore's for the cursor. The last
 * getMore will use a different explain object as n response. Uses the provided batches in batchList
 * as responses for the cursor. Checks stats and returns result of executing pipeline with explain
 * verbosity set to 'verbosity'.
 *
 * @param {MongotMock} mongotMock fixture
 * @param {DBCollection} coll
 * @param {Object} searchCmd the search query to send to mongot
 * @param {Array} batchList Array of batches that mongot should return in order.
 * @param {NumberLong} cursorId optional
 * @param {Object} vars optional - vars that mongot should return
 * @param {Object} explainContents optional - explain object that mongot should return for most
 *     responses
 * @param {Object} lastExplainContents optional - the explain object for the last getMore
 */
export function setUpMongotReturnExplainAndCursorGetMore({
    mongotMock,
    coll,
    searchCmd,
    batchList,
    cursorId = NumberLong(123),
    vars = null,
    explainContents = defaultExplainContents,
    lastExplainContents = defaultLastExplainContents,
}) {
    assert(
        batchList.length > 1,
        "Must have at least two responses in batchList: one for the initial reply, and one for the getMore.");

    const collName = coll.getName();
    const collNS = coll.getFullName();
    let history = [];

    // First response should have vars and the command.
    history.push({
        expectedCommand: searchCmd,
        response: mongotResponseForBatch(batchList[0], cursorId, collNS, 1, explainContents, vars)
    });

    for (let i = 1; i < batchList.length - 1; i++) {
        let getMoreResponse = {
            expectedCommand: {getMore: cursorId, collection: collName},
            response: mongotResponseForBatch(batchList[i], cursorId, collNS, 1, explainContents),
        };
        history.push(getMoreResponse);
    }
    // Last response should return a closed cursor and use different explain content to
    // differentiate the explain.
    const lastGetMore = {
        expectedCommand: {getMore: cursorId, collection: collName},
        response: mongotResponseForBatch(
            batchList[batchList.length - 1], NumberLong(0), collNS, 1, lastExplainContents),
    };
    history.push(lastGetMore);
    mongotMock.setMockResponses(history, cursorId);
}

/**
 * Helper function to obtain the search stage (ex. $searchMeta, $_internalSearchMongotRemote) and to
 * check its explain result. Will only check $_internalSearchIdLookup if 'nReturnedIdLookup is
 * provided.
 * @param {Object} result explain result
 * @param {string} stageType ex. "$_internalSearchMongotRemote" , "$searchMeta",
 *     "$_internalSearchIdLookup "
 * @param {string} verbosity The verbosity of explain. "nReturned" and "executionTimeMillisEstimate"
 *     will not be checked for 'queryPlanner' verbosity "
 * @param {NumberLong} nReturned The number of documents that should be returned in the
 *     searchStage.
 * @param {NumberLong} nReturnedIdLookup The number of documents that should be returned in the
 *     idLookup stage if it exists in the result.
 * @param {Object} explainContents The explain object that the stage should contain.
 */
export function getSearchStagesAndVerifyExplainOutput(
    {result, stageType, verbosity, nReturned, nReturnedIdLookup = null, explainContents}) {
    const searchStage = getAggPlanStage(result, stageType);
    assert.neq(searchStage, null, result);
    verifySearchStageExplainOutput({
        stage: searchStage,
        stageType: stageType,
        nReturned: nReturned,
        explain: explainContents,
        verbosity: verbosity,
    });

    if (nReturnedIdLookup != null) {
        const searchIdLookupStage = getAggPlanStage(result, "$_internalSearchIdLookup");
        assert.neq(searchIdLookupStage, null, result);
        verifySearchStageExplainOutput({
            stage: searchIdLookupStage,
            stageType: "$_internalSearchIdLookup",
            nReturned: nReturnedIdLookup,
            verbosity: verbosity,
        });
    }
}

/**
 * This function checks that a search stage from an explain output contains the information that
 * it should.
 * @param {Object} stage The object representing a search stage ($_internalSearchMongotRemote,
 *     $searchMeta, $_internalSearchIdLookup) from explain output.
 * @param {string} stageType ex. "$_internalSearchMongotRemote" , "$searchMeta",
 *     "$_internalSearchIdLookup "
 * @param {NumberLong} nReturned The number of documents that should be returned in the
 *     searchStage.
 * @param {Object} explain The explain object that the stage should contain.
 * @param {string} verbosity The verbosity of explain. "nReturned" and "executionTimeMillisEstimate"
 *     will not be checked for 'queryPlanner' verbosity "
 */
export function verifySearchStageExplainOutput(
    {stage, stageType, nReturned, explain = null, verbosity}) {
    if (verbosity != "queryPlanner") {
        assert(stage.hasOwnProperty("nReturned"));
        assert.eq(nReturned, stage["nReturned"]);
        assert(stage.hasOwnProperty("executionTimeMillisEstimate"));
    }

    if (explain != null) {
        const explainStage = stage[stageType];
        assert(explainStage.hasOwnProperty("explain"), explainStage);
        assert.eq(explain, explainStage["explain"]);
    }
}
