/**
 * Test that mongod correctly reports HostUnreachable when mongot is killed.
 */
import {assertErrCodeAndErrMsgContains} from "jstests/aggregation/extras/utils.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {MongotMock} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {prepCollection, prepMongotResponse} from "jstests/with_mongot/mongotmock/lib/utils.js";

const kSIGKILL = 9;
const dbName = jsTestName();
const collName = jsTestName();

// Set up mongotmock and point the mongod to it.
const mongotmock = new MongotMock();
mongotmock.start();
const mongotConn = mongotmock.getConnection();

const conn = MongoRunner.runMongod({setParameter: {mongotHost: mongotConn.host}});

prepCollection(conn, dbName, collName);
const coll = conn.getDB(dbName).getCollection(collName);
const searchQuery = {
    query: "cakes",
    path: "title"
};
const pipeline = [{$search: searchQuery}];

// First, make sure mongod can properly communicate with mongot.
{
    const collectionUUID = getUUIDFromListCollections(conn.getDB(dbName), collName);
    const expectedCommand =
        {search: collName, query: searchQuery, $db: dbName, collectionUUID: collectionUUID};
    const expected =
        prepMongotResponse(expectedCommand, coll, mongotConn, NumberLong(123) /* cursorId */);
    let cursor = coll.aggregate(pipeline, {cursor: {batchSize: 2}});
    assert.eq(expected, cursor.toArray());
}

// Next, make sure mongod properly reports HostUnreachable when mongot is uncleanly shut down.
{
    mongotmock.stop(kSIGKILL);
    // We cannot assert on a specific error message because it will vary based on the transport
    // protocol used.
    assertErrCodeAndErrMsgContains(coll, pipeline, ErrorCodes.HostUnreachable, "");
}

MongoRunner.stopMongod(conn);
