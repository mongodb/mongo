/**
 * Test the use of "explain" with the "$search" aggregation stage in SBE.
 * @tags: [featureFlagSbeFull]
 */
import {planHasStage} from "jstests/libs/analyze_plan.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {MongotMock} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

// Set up mongotmock and point the mongod to it.
const mongotmock = new MongotMock();
mongotmock.start();
const mongotConn = mongotmock.getConnection();

const conn = MongoRunner.runMongod(
    {setParameter: {mongotHost: mongotConn.host, featureFlagSearchInSbe: true}});
const db = conn.getDB("test");
const coll = db[jsTestName()];
coll.drop();

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
    assert.eq(planHasStage(db, result, 'SEARCH'), true, result);

    const winningPlan = result.queryPlanner.winningPlan;
    assert(winningPlan.hasOwnProperty("slotBasedPlan"), winningPlan);
    assert(winningPlan.slotBasedPlan.stages.includes('search_cursor'), winningPlan.slotBasedPlan);

    assert(winningPlan.hasOwnProperty("remotePlans"), winningPlan);
    assert(winningPlan.remotePlans.length == 1, winningPlan.remotePlans);

    const searchExplain = winningPlan.remotePlans[0];
    assert(searchExplain.hasOwnProperty("explain"), searchExplain);
    assert(searchExplain.hasOwnProperty("mongotQuery"), searchExplain);
    assert.eq(explainContents, searchExplain["explain"]);
    assert.eq(searchQuery, searchExplain["mongotQuery"]);
}

MongoRunner.stopMongod(conn);
mongotmock.stop();
