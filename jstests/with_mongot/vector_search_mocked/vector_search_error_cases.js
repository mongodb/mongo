/**
 * Test error conditions for the `$vectorSearch` aggregation pipeline stage.
 * @tags: [
 *  requires_fcv_71,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {
    MongotMock,
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

// Start mock mongot.
const mongotMock = new MongotMock();
mongotMock.start();
const mockConn = mongotMock.getConnection();

const rst = new ReplSetTest({nodes: 1, nodeOptions: {setParameter: {mongotHost: mockConn.host}}});
rst.startSet();
rst.initiate();

const dbName = jsTestName();
const collName = jsTestName();
const testDB = rst.getPrimary().getDB(dbName);
testDB.dropDatabase();

const coll = testDB.getCollection(collName);
coll.insert({_id: 0});
coll.insert({_id: 1});
coll.insert({_id: 2});

function makeVectorSearchStage() {
    return {$vectorSearch: {queryVector: [], path: "x", numCandidates: 1, limit: 1}};
}

function runPipeline(pipeline) {
    return testDB.runCommand({aggregate: collName, pipeline, cursor: {}});
}

// $match cannot proceed $vectorSearch.
assert.commandFailedWithCode(runPipeline([{$match: {}}, makeVectorSearchStage()]), 40602);

// $search and $vectorSearch are not allowed in the same pipeline.
assert.commandFailedWithCode(runPipeline([{$search: {}}, makeVectorSearchStage()]), 40602);
assert.commandFailedWithCode(runPipeline([makeVectorSearchStage(), {$search: {}}]), 40602);

// $vectorSearch must have a non-negative limit.
assert.commandFailedWithCode(
    runPipeline([{$vectorSearch: {queryVector: [], path: "x", numCandidates: 1, limit: -1}}]),
    7912700);

// $vectorSearch is not allowed in a sub-pipeline.
assert.commandFailedWithCode(
    runPipeline([{$unionWith: {coll: collName, pipeline: [makeVectorSearchStage()]}}]), 31441);
assert.commandFailedWithCode(
    runPipeline([{$lookup: {from: collName, pipeline: [makeVectorSearchStage()], as: "lookup1"}}]),
    51047);
assert.commandFailedWithCode(runPipeline([{$facet: {originalPipeline: [makeVectorSearchStage()]}}]),
                             40600);

// $vectorSearch does not support $SEARCH_META.
assert.commandFailedWithCode(
    runPipeline([makeVectorSearchStage(), {$project: {_id: 1, meta: "$$SEARCH_META"}}]), 6347902);

// $vectorSearch cannot be used inside a transaction.
let session = testDB.getMongo().startSession({readConcern: {level: "local"}});
let sessionDb = session.getDatabase(dbName);

session.startTransaction();
assert.commandFailedWithCode(
    sessionDb.runCommand({aggregate: collName, pipeline: [makeVectorSearchStage()], cursor: {}}),
    ErrorCodes.OperationNotSupportedInTransaction);
session.endSession();

// $search is not allowed in an update pipeline.
assert.commandFailedWithCode(
    testDB.runCommand({"findandmodify": collName, "update": [makeVectorSearchStage()]}),
    ErrorCodes.InvalidOptions);

// $vectorSearch is not valid on views.
assert.commandWorked(
    testDB.runCommand({create: "idView", viewOn: collName, pipeline: [{$match: {_id: {$gt: 1}}}]}));
assert.commandFailedWithCode(
    testDB.runCommand({aggregate: 'idView', pipeline: [makeVectorSearchStage()], cursor: {}}),
    40602);

mongotMock.stop();
rst.stopSet();
