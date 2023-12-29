/**
 * Tests that $search queries release locks while waiting for network requests.
 *
 * This assertion is made by starting up a number of threads equal to the number of wiredTiger
 * storage tickets available, having each run a $search query, and letting them all block while
 * waiting for the mongot network response by setting a failpoint causing them to hang at this
 * point. With these threads all blocked waiting, we should still be able to run a concurrent query.
 * This would not be true if the threads do not drop their locks/resources while waiting.
 */
import {Thread} from "jstests/libs/parallelTester.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {MongotMock} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

const numThreads = 8;
let threads = [];

const searchQuery = {
    query: "cakes",
    path: "title"
};

const dbName = jsTestName();
const collName = jsTestName();

function launchSearchQuery(mongod, threads, {times}) {
    jsTestLog("Starting " + times + " connections");
    for (let i = 0; i < times; i++) {
        let thread = new Thread(function(connStr, dbName, collName, searchQuery) {
            const client = new Mongo(connStr);
            const db = client.getDB(dbName);
            const coll = db[collName];

            // We already set up the mongotmock responses below, so this can be run without doing
            // anything else.
            const results = coll.aggregate([{$search: searchQuery}]).toArray();
            assert.eq(results, [
                {"_id": 1, "title": "cakes"},
                {"_id": 2, "title": "cookies and cakes"},
                {"_id": 3, "title": "vegetables"}
            ]);
        }, mongod.host, dbName, collName, searchQuery);
        thread.start();
        threads.push(thread);
    }
}

// Set up mongotmock.
const mongotmock = new MongotMock();
mongotmock.start();
const mongotConn = mongotmock.getConnection();

// Start a mongod normally, and point it at the mongot. Configure the mongod <--> mongod
// connection pool to have appropriate size limits.
let conn = MongoRunner.runMongod({
    setParameter: {
        mongotHost: mongotConn.host,
        storageEngineConcurrencyAdjustmentAlgorithm: "fixedConcurrentTransactions"
    }
});

assert.commandWorked(
    conn.adminCommand({setParameter: 1, wiredTigerConcurrentReadTransactions: numThreads}));

const db = conn.getDB(dbName);
const coll = db[collName];

assert.commandWorked(coll.insert({"_id": 1, "title": "cakes"}));
assert.commandWorked(coll.insert({"_id": 2, "title": "cookies and cakes"}));
assert.commandWorked(coll.insert({"_id": 3, "title": "vegetables"}));

const collectionUUID = getUUIDFromListCollections(db, coll.getName());
const searchCmd = {
    search: coll.getName(),
    collectionUUID,
    query: searchQuery,
    $db: dbName
};

// Set up mongotmock with expected responses and turn on the failpoint.
assert.commandWorked(mongotmock.getConnection().adminCommand(
    {configureFailPoint: "mongotWaitBeforeRespondingToQuery", mode: "alwaysOn"}));

for (let i = 1; i <= numThreads; ++i) {
    const cursorId = NumberLong(i);
    const history = [
        {
            expectedCommand: searchCmd,
            response: {
                cursor: {
                    id: cursorId,
                    ns: coll.getFullName(),
                    nextBatch: [
                        {_id: 1, $searchScore: 0.987},
                        {_id: 2, $searchScore: 0.654},
                    ]
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
                    nextBatch: [{_id: 3, $searchScore: 0.321}]
                },
            }
        },
    ];

    assert.commandWorked(
        mongotConn.adminCommand({setMockResponses: 1, cursorId: cursorId, history: history}));
}

// Launch a bunch of blocked search queries.
launchSearchQuery(conn, threads, {times: numThreads});

// Wait for them to block on the mongot request.
assert.commandWorked(mongotmock.getConnection().adminCommand({
    waitForFailPoint: "mongotWaitBeforeRespondingToQuery",
    timesEntered: NumberLong(numThreads),
    maxTimeMS: NumberLong(100000)
}));

// Make sure we can still run another read query concurrently.
assert(coll.findOne(), "Expected to be able to run a concurrent query");

// Now turn off the fail point and let the $search queries complete.
assert.commandWorked(mongotmock.getConnection().adminCommand(
    {configureFailPoint: "mongotWaitBeforeRespondingToQuery", mode: "off"}));

for (const thread of threads) {
    thread.join();
}

MongoRunner.stopMongod(conn);
mongotmock.stop();