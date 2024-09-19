/**
 * Utility functions for mongot tests.
 */

import {
    getAggPlanStage,
} from "jstests/libs/analyze_plan.js";
import {
    mongotMultiCursorResponseForBatch,
    mongotResponseForBatch
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

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

export function getDefaultExplainContents() {
    return defaultExplainContents;
}

// Explain object used for the last explain returned.
const defaultLastExplainContents = {
    profession: "teacher"
};

export function getDefaultLastExplainContents() {
    return defaultLastExplainContents;
}

/**
 * Mocks mongot returning an explain object only to a search query.
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
 * Mocks mongot returning an explain object and exhausted cursor to a search query. Uses provided
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
 * Mocks mongot returning an explain object and exhausted cursor to a sharded search query. Uses
 * nextBatch for results cursor response, and metaBatch for metadata cursor response.
 *
 * @param {MongotMock} mongotMock mongotMock for specific shard
 * @param {DBCollection} coll
 * @param {Object} searchCmd the search query to send to mongot
 * @param {Array{Object}} nextBatch batch of results that mongot should return for results cursor
 * @param {Array{Object}} metaBatch batch of meta documents that mongot should return for metadata
 *     cursor.
 * @param {NumberLong} cursorId optional
 * @param {NumberLong} metaCursorId optional
 * @param {Object} explainContents optional - explain object that mongot should return
 */
export function setUpMongotReturnExplainAndMultiCursor({
    mongotMock,
    coll,
    searchCmd,
    nextBatch,
    metaBatch,
    cursorId = NumberLong(123),
    metaCursorId = NumberLong(1230),
    explainContents = defaultLastExplainContents,
}) {
    let history = [{
        expectedCommand: searchCmd,
        response: mongotMultiCursorResponseForBatch(nextBatch,
                                                    NumberLong(0),
                                                    metaBatch,
                                                    NumberLong(0),
                                                    coll.getFullName(),
                                                    1,
                                                    explainContents /*results explain*/,
                                                    explainContents /*meta explain*/)
    }];
    mongotMock.setMockResponses(history, cursorId, metaCursorId);
}

/**
 * Helper to set up getMores for history. Adds a getMore to history starting from the 2nd entry
 * in batchList.
 */
function setUpGetMoreHistory(
    history, batchList, collName, collNS, cursorId, explainContents, lastExplainContents) {
    assert(batchList.length > 1, "To set up getMore's, batchList must have at least 2 responses.");
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
    return history;
}

/**
 * Mocks mongot returning an explain object and cursor with getMore's for the cursor in an unsharded
 * scenario. The last getMore will use a different explain object as an response.
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

    history = setUpGetMoreHistory(
        history, batchList, collName, collNS, cursorId, explainContents, lastExplainContents);
    mongotMock.setMockResponses(history, cursorId);
}

/**
 * Mocks mongot returning an explain object and cursor with getMore's for results and/or metadata
 * cursor. The results cursor uses batchList and the metadata cursor uses metaBatchList. The last
 * getMore for each cursor will use a different explain object as a response.
 *
 * @param {MongotMock} mongotMock mongotMock for specific shard
 * @param {DBCollection} coll
 * @param {Object} searchCmd the search query to send to mongot
 * @param {Array} batchList Array of batches that mongot should return in order for results cursor.
 * @param {Array} metaBatchList Array of batches that mongot should return in order for metadata
 *     cursor.
 * @param {NumberLong} cursorId optional
 * @param {NumberLong} metaCursorId optional
 * @param {Object} explainContents optional - explain object that mongot should return for most
 *     responses
 * @param {Object} lastExplainContents optional - the explain object for the last getMore
 */
export function setUpMongotReturnExplainAndMultiCursorGetMore({
    mongotMock,
    coll,
    searchCmd,
    batchList,
    metaBatchList,
    cursorId = NumberLong(123),
    metaCursorId = NumberLong(1230),
    explainContents = defaultExplainContents,
    lastExplainContents = defaultLastExplainContents,
}) {
    const collName = coll.getName();
    const collNS = coll.getFullName();

    // Will contain first response for both cursors and then getMores for results cursor.
    let history = [];
    // Will contain getMores for the meta cursor.
    let metaHistory = [];

    let response = mongotMultiCursorResponseForBatch(
        batchList[0],
        batchList.length == 1 ? NumberLong(0) : cursorId,
        metaBatchList[0],
        metaBatchList.length == 1 ? NumberLong(0) : metaCursorId,
        coll.getFullName(),
        1,
        batchList.length == 1 ? lastExplainContents : explainContents,
        metaBatchList.length == 1 ? lastExplainContents : explainContents);

    history.push({expectedCommand: searchCmd, response});

    if (batchList.length > 1) {
        history = setUpGetMoreHistory(
            history, batchList, collName, collNS, cursorId, explainContents, lastExplainContents);
    }
    mongotMock.setMockResponses(history, cursorId, metaCursorId);

    // Set up metaHistory for "meta" cursor.
    if (metaBatchList.length > 1) {
        metaHistory = setUpGetMoreHistory(metaHistory,
                                          metaBatchList,
                                          collName,
                                          collNS,
                                          metaCursorId,
                                          explainContents,
                                          lastExplainContents);
        mongotMock.setMockResponses(metaHistory, metaCursorId);
    }
}

/**
 * Helper function to obtain the search stage (ex. $searchMeta, $_internalSearchMongotRemote) and to
 * check its explain result. Will only check $_internalSearchIdLookup if 'nReturnedIdLookup is
 * provided. Function will fail for non-search stages.
 * @param {Object} result the results from running coll.explain().aggregate([[$search: ....], ...])
 * @param {string} stageType ex. "$_internalSearchMongotRemote" , "$searchMeta",
 *     "$_internalSearchIdLookup "
 * @param {string} verbosity The verbosity of explain. "nReturned" and "executionTimeMillisEstimate"
 *     will not be checked for 'queryPlanner' verbosity "
 * @param {NumberLong} nReturned The number of documents that should be returned in the
 *     searchStage.
 * @param {Object} explainContents The explain object that the stage should contain.
 */
export function getSearchStagesAndVerifyExplainOutput(
    {result, stageType, verbosity, nReturned, explainObject = null}) {
    const searchStage = getAggPlanStage(result, stageType);
    assert.neq(searchStage, null, result);
    verifySearchStageExplainOutput({
        stage: searchStage,
        stageType: stageType,
        nReturned: nReturned,
        explain: explainObject,
        verbosity: verbosity,
    });
}

/**
 * Helper function for sharded clusters to obtain the stage specified. Will check nReturned for non
 * "queryPlanner" verbosity, and explainContents if specified. This function will fail for
 * non-search stages.
 *
 * @param {Object} result the results from running coll.explain().aggregate([[$search: ....], ...])
 * @param {string} stageType ex. "$_internalSearchMongotRemote" , "$searchMeta",
 *     "$_internalSearchIdLookup "
 * @param {Integer} expectedNumStages The number of aggPlanStages expected for the stageType,
 *     generally one per shard.
 * @param {string} verbosity The verbosity of explain. "nReturned" and "executionTimeMillisEstimate"
 *     will not be checked for 'queryPlanner' verbosity "
 * @param {Array{NumberLong}} nReturnedList Index i should be nReturned for shard i. Length must
 *     match expectedNumStages.
 * @param {Object} expectedExplainContents Optional - The explain object that the stage should
 *     contain.
 */
export function getShardedSearchStagesAndVerifyExplainOutput(
    {result, stageType, expectedNumStages, verbosity, nReturnedList, expectedExplainContents}) {
    assert.eq(nReturnedList.length, expectedNumStages);
    assert(result.hasOwnProperty("shards"),
           tojson(result) + "has no shards property, but it should.");

    let counter = 0;
    // Since the explain object shard results are unordered, we manually check which shard
    // we are currently checking.
    for (let elem in result.shards) {
        // Get the number at the end of the string for which shard to check, since shards are
        // labeled rs0/rs1/etc.
        let index = parseInt(elem.match(/\d+$/)[0], 10);
        jsTestLog("Checking shard " + index);
        let stage = null;
        // We need to look through the stages for the stage that matches the stageType.
        let stages = result.shards[elem].stages;
        for (let i = 0; i < stages.length; i++) {
            let properties = Object.getOwnPropertyNames(stages[i]);
            if (properties[0] === stageType) {
                stage = stages[i];
            }
        }
        assert(stage,
               "Unable to find stageType: " + stageType + " for shard " + index +
                   " in result: " + tojson(result));

        verifySearchStageExplainOutput({
            stage: stage,
            stageType: stageType,
            nReturned: nReturnedList[index],
            explain: expectedExplainContents,
            verbosity: verbosity,
        });
        counter++;
    }

    assert.eq(counter,
              expectedNumStages,
              "Number of shards checked: " + counter + " did not match the expectedNumber: " +
                  expectedNumStages + " in the result: " + tojson(result));
}

const searchStages =
    ["$_internalSearchMongotRemote", "$searchMeta", "$_internalSearchIdLookup", "$vectorSearch"];

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
    assert(searchStages.includes(stageType),
           "stageType must be a search stage found in searchStages.");
    assert(stage[stageType],
           "Given stage isn't the expected stage. " + stageType + " is not found.");

    if (verbosity != "queryPlanner") {
        assert(stage.hasOwnProperty("nReturned"));
        assert.eq(nReturned, stage["nReturned"]);
        assert(stage.hasOwnProperty("executionTimeMillisEstimate"));
    }

    if (stageType != "$_internalSearchIdLookup") {
        assert(explain, "Explain is null but needs to be provided for initial search stage.");
    }
    if (explain != null) {
        const explainStage = stage[stageType];
        assert(explainStage.hasOwnProperty("explain"), explainStage);
        assert.eq(explain, explainStage["explain"]);
    }
}
