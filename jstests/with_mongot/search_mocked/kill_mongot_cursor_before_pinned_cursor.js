/**
 * Tests that when stopping the server the task executor pinned to the mongot cursor isn't killed
 * before the underlying cursor gets killed.
 */
import {Thread} from "jstests/libs/parallelTester.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {MongotMock} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

const searchQuery = {
    query: "cakes",
    path: "title",
};

const dbName = jsTestName();
const collName = jsTestName();

// Set up mongotmock.
const mongotmock = new MongotMock();
mongotmock.start();
const mongotConn = mongotmock.getConnection();

// Start a mongod normally, and point it at the mongot. Configure the mongod <--> mongot connection
// pool to have appropriate size limits.
let conn = MongoRunner.runMongod({
    setParameter: {
        mongotHost: mongotConn.host,
        storageEngineConcurrencyAdjustmentAlgorithm: "fixedConcurrentTransactions",
    },
});

assert.commandWorked(conn.adminCommand({setParameter: 1, wiredTigerConcurrentReadTransactions: 1}));

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
    $db: dbName,
};

// Set up mongotmock with expected responses and turn on the failpoint.
assert.commandWorked(
    mongotmock
        .getConnection()
        .adminCommand({configureFailPoint: "mongotWaitBeforeRespondingToQuery", mode: "alwaysOn"}),
);
const cursorId = NumberLong(1);
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
                ],
            },
            ok: 1,
        },
        maybeUnused: true,
    },
    {
        expectedCommand: {getMore: cursorId, collection: coll.getName()},
        response: {
            ok: 1,
            cursor: {
                id: NumberLong(0),
                ns: coll.getFullName(),
                nextBatch: [{_id: 3, $searchScore: 0.321}],
            },
        },
        maybeUnused: true,
    },
];

assert.commandWorked(mongotConn.adminCommand({setMockResponses: 1, cursorId: cursorId, history: history}));

// Launch a blocked search query.
let thread = new Thread(
    function (connStr, dbName, collName, searchQuery) {
        const client = new Mongo(connStr);
        const db = client.getDB(dbName);
        const coll = db[collName];

        // During shutdown, this can throw. Accept network/shutdown errors.
        try {
            const results = coll.aggregate([{$search: searchQuery}]).toArray();
            // If we didn't shut down early, verify results.
            assert.eq(results, [
                {"_id": 1, "title": "cakes"},
                {"_id": 2, "title": "cookies and cakes"},
                {"_id": 3, "title": "vegetables"},
            ]);
        } catch (e) {
            // Expected when mongod is stopped while the query is pinned.
            const acceptableErrorCodes = [
                ErrorCodes.InterruptedAtShutdown,
                ErrorCodes.CallbackCanceled,
                ErrorCodes.ShutdownInProgress,
            ];
            if (!acceptableErrorCodes.includes(e.code) && !isNetworkError(e.code)) {
                throw e;
            }
        }
    },
    conn.host,
    dbName,
    collName,
    searchQuery,
);
thread.start();

// Wait for it to block on the mongot request.
assert.commandWorked(
    mongotmock.getConnection().adminCommand({
        waitForFailPoint: "mongotWaitBeforeRespondingToQuery",
        timesEntered: NumberLong(1),
        maxTimeMS: NumberLong(1000000),
    }),
);

// Stop the server, which triggers the shutdown of the pinned cursor.
MongoRunner.stopMongod(conn);
thread.join();
// Now turn off the fail point and let the $search queries complete.
assert.commandWorked(
    mongotmock.getConnection().adminCommand({configureFailPoint: "mongotWaitBeforeRespondingToQuery", mode: "off"}),
);
mongotmock.stop();
