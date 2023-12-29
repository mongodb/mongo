/**
 * Verify that `$searchMeta` extracts SEARCH_META variable returned by mongot even if no docs
 * returned.
 */
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
// We don't need documents for this test, but we need to create the collection. MongotMock will
// still not return any documents.
assert.commandWorked(coll.insert({"_id": 1, "title": "cakes"}));

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
const cursorId = NumberLong(17);

// Verify that $searchMeta evaluates into SEARCH_META variable returned by mongot if no docs are
// returned.
{
    const history = [{
        expectedCommand: searchCmd,
        response: {
            ok: 1,
            cursor: {id: NumberLong(0), ns: coll.getFullName(), nextBatch: []},
            vars: {SEARCH_META: {value: 42}}
        }
    }];
    assert.commandWorked(
        mongotConn.adminCommand({setMockResponses: 1, cursorId, history: history}));

    let cursorMeta = coll.aggregate([{$searchMeta: searchQuery}], {cursor: {}});
    const expectedMeta = [{value: 42}];
    assert.eq(expectedMeta, cursorMeta.toArray());
}

MongoRunner.stopMongod(conn);
mongotmock.stop();
