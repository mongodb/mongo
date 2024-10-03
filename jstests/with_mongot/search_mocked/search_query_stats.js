/**
 * Verify the query shape that is outputted by $queryStats for $search and $searchMeta queries.
 */
import {getQueryStats} from "jstests/libs/query/query_stats_utils.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {MongotMock} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

// Set up mongotmock and point the mongod to it.
const mongotmock = new MongotMock();
mongotmock.start();
const mongotConn = mongotmock.getConnection();

const conn = MongoRunner.runMongod(
    {setParameter: {mongotHost: mongotConn.host, internalQueryStatsRateLimit: -1}});

const dbName = jsTestName();
const testDB = conn.getDB(dbName);

const coll = testDB.search_query_stats;
coll.drop();
assert.commandWorked(coll.insert({"_id": 1, "title": "cakes"}));
assert.commandWorked(coll.insert({"_id": 2, "title": "cookies and cakes"}));
assert.commandWorked(coll.insert({"_id": 3, "title": "vegetables"}));

const collUUID = getUUIDFromListCollections(testDB, coll.getName());
const searchQuery = {
    query: "cakes",
    path: "title",
    near: {path: "released", origin: ISODate("2011-09-01T00:00:00"), pivot: 7776000000}
};

const searchCmd = {
    search: coll.getName(),
    collectionUUID: collUUID,
    query: searchQuery,
    $db: dbName
};

const cursorId = NumberLong(17);
const history = [{
    expectedCommand: searchCmd,
    response: {
        ok: 1,
        cursor: {id: NumberLong(0), ns: coll.getFullName(), nextBatch: []},
        vars: {SEARCH_META: {value: 42}}
    }
}];

// Test for $searchMeta
assert.commandWorked(mongotConn.adminCommand({setMockResponses: 1, cursorId, history: history}));
coll.aggregate([{$searchMeta: searchQuery}], {cursor: {}});
let stats = getQueryStats(conn);
assert.eq(1, stats.length);
let queryShape = stats[0]["key"]["queryShape"];
assert.eq(queryShape["pipeline"], [{"$searchMeta": "?object"}], queryShape);

// Test for $search
assert.commandWorked(
    mongotConn.adminCommand({setMockResponses: 1, cursorId: cursorId, history: history}));
coll.aggregate([{$search: searchQuery}], {cursor: {}});
stats = getQueryStats(conn);
assert.eq(3, stats.length);
queryShape = stats[1]["key"]["queryShape"];
assert.eq(queryShape["pipeline"], [{"$search": "?object"}], queryShape);

MongoRunner.stopMongod(conn);
mongotmock.stop();
