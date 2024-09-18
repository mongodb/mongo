/**
 * Test error conditions for the `$search` aggregation pipeline stages.
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

const dbName = "test";
const collName = "search_error_cases";
const testDB = rst.getPrimary().getDB(dbName);
testDB.dropDatabase();

assert.commandWorked(testDB[collName].insert({_id: 0}));
assert.commandWorked(testDB[collName].insert({_id: 1}));
assert.commandWorked(testDB[collName].insert({_id: 2}));

// $search cannot be used inside a transaction.
let session = testDB.getMongo().startSession({readConcern: {level: "local"}});
let sessionDb = session.getDatabase(dbName);

session.startTransaction();
assert.commandFailedWithCode(
    sessionDb.runCommand({aggregate: collName, pipeline: [{$search: {}}], cursor: {}}),
    ErrorCodes.OperationNotSupportedInTransaction);
session.endSession();

// $search cannot be used inside a $facet subpipeline.
let pipeline = [{$facet: {originalPipeline: [{$search: {}}]}}];
let cmdObj = {aggregate: collName, pipeline: pipeline, cursor: {}};

assert.commandFailedWithCode(testDB.runCommand(cmdObj), 40600);

// $search cannot be used inside a transaction.
session = testDB.getMongo().startSession({readConcern: {level: "local"}});
sessionDb = session.getDatabase(dbName);
session.startTransaction();
assert.commandFailedWithCode(
    sessionDb.runCommand({aggregate: collName, pipeline: [{$search: {}}], cursor: {}}),
    ErrorCodes.OperationNotSupportedInTransaction);
session.endSession();

// $search is only valid as the first stage in a pipeline.
assert.commandFailedWithCode(testDB.runCommand({
    aggregate: collName,
    pipeline: [{$match: {}}, {$search: {}}],
    cursor: {},
}),
                             40602);

// $search is not allowed in an update pipeline. Error code matters on version.
assert.commandFailedWithCode(
    testDB.runCommand({"findandmodify": collName, "update": [{"$search": {}}]}),
    [6600901, ErrorCodes.InvalidOptions]);

// Make sure the server is still up.
assert.commandWorked(testDB.runCommand("ping"));

// $search is not valid on views.
assert.commandWorked(
    testDB.runCommand({create: "idView", viewOn: collName, pipeline: [{$match: {_id: {$gt: 1}}}]}));

const searchQuery = {
    query: "1",
    path: "_id"
};

assert.commandFailedWithCode(
    testDB.runCommand({aggregate: 'idView', pipeline: [{$search: searchQuery}], cursor: {}}),
    40602);

// Assert the oversubscription factor cannot be configured to any value less than 1.
assert.commandFailedWithCode(
    testDB.adminCommand(
        {setClusterParameter: {internalSearchOptions: {oversubscriptionFactor: 0.9}}}),
    ErrorCodes.BadValue);
assert.commandFailedWithCode(
    testDB.adminCommand(
        {setClusterParameter: {internalSearchOptions: {oversubscriptionFactor: 0}}}),
    ErrorCodes.BadValue);
assert.commandFailedWithCode(
    testDB.adminCommand(
        {setClusterParameter: {internalSearchOptions: {oversubscriptionFactor: -5}}}),
    ErrorCodes.BadValue);

// Assert the batchSize growth factor cannot be configured to any value less than 1.
assert.commandFailedWithCode(
    testDB.adminCommand(
        {setClusterParameter: {internalSearchOptions: {batchSizeGrowthFactor: 0.9}}}),
    ErrorCodes.BadValue);
assert.commandFailedWithCode(
    testDB.adminCommand({setClusterParameter: {internalSearchOptions: {batchSizeGrowthFactor: 0}}}),
    ErrorCodes.BadValue);
assert.commandFailedWithCode(
    testDB.adminCommand(
        {setClusterParameter: {internalSearchOptions: {batchSizeGrowthFactor: -5}}}),
    ErrorCodes.BadValue);

mongotMock.stop();
rst.stopSet();
