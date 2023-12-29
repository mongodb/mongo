/**
 * Test the use of "explain" with the "$search" aggregation stage.
 */
import {getAggPlanStage} from "jstests/libs/analyze_plan.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {checkSbeRestrictedOrFullyEnabled} from "jstests/libs/sbe_util.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {MongotMock} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

// Set up mongotmock and point the mongod to it.
const mongotmock = new MongotMock();
mongotmock.start();
const mongotConn = mongotmock.getConnection();

const conn = MongoRunner.runMongod({setParameter: {mongotHost: mongotConn.host}});
const db = conn.getDB("test");
const coll = db.search;
coll.drop();

if (checkSbeRestrictedOrFullyEnabled(db) &&
    FeatureFlagUtil.isPresentAndEnabled(db.getMongo(), 'SearchInSbe')) {
    jsTestLog("Skipping the test because it only applies to $search in classic engine.");
    MongoRunner.stopMongod(conn);
    mongotmock.stop();
    quit();
}

assert.commandWorked(coll.insert({_id: 1, name: "Chekhov"}));
assert.commandWorked(coll.insert({_id: 2, name: "Dostoevsky"}));
assert.commandWorked(coll.insert({_id: 5, name: "Pushkin"}));
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
const cursorId = NumberLong(123);

for (const currentVerbosity of ["queryPlanner", "executionStats", "allPlansExecution"]) {
    const searchCmd = {
        search: coll.getName(),
        collectionUUID: collUUID,
        query: searchQuery,
        explain: {verbosity: currentVerbosity},
        $db: "test"
    };
    // Give mongotmock some stuff to return.
    {
        const history = [{expectedCommand: searchCmd, response: {explain: explainContents, ok: 1}}];
        assert.commandWorked(
            mongotConn.adminCommand({setMockResponses: 1, cursorId: cursorId, history: history}));
    }
    const result = coll.explain(currentVerbosity).aggregate([{$search: searchQuery}]);

    const searchStage = getAggPlanStage(result, "$_internalSearchMongotRemote");
    assert.neq(searchStage, null, result);
    const stage = searchStage["$_internalSearchMongotRemote"];
    assert(stage.hasOwnProperty("explain"), stage);
    assert.eq(explainContents, stage["explain"]);
}

MongoRunner.stopMongod(conn);
mongotmock.stop();
