/**
 * Verify that `$searchMeta` extracts SEARCH_META variable returned by mongot.
 */
import {getPlanStages} from "jstests/libs/analyze_plan.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {checkSbeRestrictedOrFullyEnabled} from "jstests/libs/sbe_util.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {MongotMock} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

// Set up mongotmock and point the mongod to it.
const mongotmock = new MongotMock();
mongotmock.start();
const mongotConn = mongotmock.getConnection();

const conn = MongoRunner.runMongod({setParameter: {mongotHost: mongotConn.host}});

const dbName = jsTestName();
const testDB = conn.getDB(dbName);

const coll = testDB.searchCollector;
coll.drop();
assert.commandWorked(coll.insert({"_id": 1, "title": "cakes"}));
assert.commandWorked(coll.insert({"_id": 2, "title": "cookies and cakes"}));
assert.commandWorked(coll.insert({"_id": 3, "title": "vegetables"}));

const collUUID = getUUIDFromListCollections(testDB, coll.getName());
const searchQuery = {
    query: "cakes",
    path: "title"
};

const searchCmd = {
    search: coll.getName(),
    collectionUUID: collUUID,
    query: searchQuery,
    $db: dbName
};
const explainContents = {
    content: "test"
};
const cursorId = NumberLong(17);

// Verify that $searchMeta evaluates into SEARCH_META variable returned by mongot.
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
            vars: {SEARCH_META: {value: 42}}
        }
    }];
    assert.commandWorked(
        mongotConn.adminCommand({setMockResponses: 1, cursorId, history: history}));

    let cursorMeta = coll.aggregate([{$searchMeta: searchQuery}], {cursor: {}});
    const expectedMeta = [{value: 42}];
    assert.eq(expectedMeta, cursorMeta.toArray());
}

{
    const history = [{expectedCommand: searchCmd, response: {explain: explainContents, ok: 1}}];

    assert.commandWorked(
        mongotConn.adminCommand({setMockResponses: 1, cursorId, history: history}));

    const explain = coll.explain("queryPlanner").aggregate([{$searchMeta: searchQuery}]);

    if (checkSbeRestrictedOrFullyEnabled(testDB) &&
        FeatureFlagUtil.isPresentAndEnabled(testDB.getMongo(), 'SearchInSbe')) {
        const winningPlan = explain.queryPlanner.winningPlan;
        const searchPlan = getPlanStages(winningPlan.queryPlan, "SEARCH");
        assert.eq(1, searchPlan.length, searchPlan);
        assert(searchPlan[0].hasOwnProperty("isSearchMeta"), searchPlan);
        assert(searchPlan[0]["isSearchMeta"], searchPlan);

        assert.eq(1, winningPlan.remotePlans.length, winningPlan);
        const remotePlan = winningPlan.remotePlans[0];
        assert.eq(explainContents, remotePlan.explain, remotePlan);
    } else {
        assert(explain.stages[0].hasOwnProperty("$searchMeta"), explain.stages);
    }
}

// Verify that the count from SEARCH_META winds up in the slow query logs even on a normal search
// command.
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
            vars: {
                SEARCH_META: {
                    value: 42,
                    "count": {"lowerBound": 3},
                    "slowQueryLog": {"msg": "Arbitrary payload"}
                }
            }
        }
    }];
    assert.commandWorked(
        mongotConn.adminCommand({setMockResponses: 1, cursorId, history: history}));

    // Make sure we capture all queries in slow logs.
    testDB.runCommand({profile: 0, slowms: -1});
    let _ = coll.aggregate([{$search: searchQuery}], {cursor: {}});
    const logs = assert.commandWorked(testDB.adminCommand({getLog: "global"}));
    assert(logs["log"], "no log field");
    const arrayLog = logs["log"];
    assert.gt(arrayLog.length, 0, "no log lines");
    assert(arrayLog.some(function(v) {
        return v.includes("Slow query") && v.includes("resultCount");
    }));
    assert(arrayLog.some(function(v) {
        return v.includes("Slow query") && v.includes("slowQueryLog");
    }));
    testDB.runCommand({profile: 0, slowms: 200});
}

MongoRunner.stopMongod(conn);
mongotmock.stop();
