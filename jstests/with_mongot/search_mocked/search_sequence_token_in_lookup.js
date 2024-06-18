/**
 * Test that searchSequenceToken in $lookup in standalone environment works in classic and SBE.
 */
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {MongotMock} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

// Set up mongotmock and point the mongod to it.
const mongotmockSBE = new MongotMock();
mongotmockSBE.start();
const mongotConnSBE = mongotmockSBE.getConnection();

const connSBE = MongoRunner.runMongod({setParameter: {mongotHost: mongotConnSBE.host}});
const dbSBE = connSBE.getDB("test");
const collSBE = dbSBE[jsTestName()];
collSBE.drop();

function insertDocs(coll) {
    assert.commandWorked(coll.insert({"_id": 1, "test": 100}));
    assert.commandWorked(coll.insert({"_id": 2, "test": 100}));
    assert.commandWorked(coll.insert({"_id": 3, "test": 1}));
    assert.commandWorked(coll.insert({"_id": 4, "test": 10}));
    assert.commandWorked(coll.insert({"_id": 5, "test": 100}));
    assert.commandWorked(coll.insert({"_id": 6, "test": 100}));
    assert.commandWorked(coll.insert({"_id": 7, "test": 10}));
    assert.commandWorked(coll.insert({"_id": 8, "test": 100}));
}

insertDocs(collSBE);

function testPaginationInLookup(mongotConn, db, coll) {
    const searchQuery = {query: "cake", path: "titles"};

    let collUUID = getUUIDFromListCollections(db, coll.getName());
    const searchCmd = {
        search: coll.getName(),
        collectionUUID: collUUID,
        query: searchQuery,
        $db: "test",
        cursorOptions: {requiresSearchSequenceToken: true}
    };

    const cursorId = NumberLong(124);
    const historyObj = {
        expectedCommand: searchCmd,
        response: {
            cursor: {
                id: NumberLong(0),
                ns: coll.getFullName(),
                nextBatch: [
                    {_id: 1, $searchSequenceToken: "aaaaaaa=="},
                    {_id: 2, $searchSequenceToken: "bbbbbbb=="},
                    {_id: 3, $searchSequenceToken: "ccccccc=="},
                    {_id: 4, $searchSequenceToken: "ddddddd=="},
                    {_id: 5, $searchSequenceToken: "eeeeeee=="},
                    {_id: 6, $searchSequenceToken: "fffffff=="},
                    {_id: 7, $searchSequenceToken: "ggggggg=="},
                    {_id: 8, $searchSequenceToken: "hhhhhhh=="},
                ]
            },
            ok: 1
        }
    };

    let history = [historyObj];
    for (let i = 0; i < 8; i++) {
        assert.commandWorked(mongotConn.adminCommand(
            {setMockResponses: 1, cursorId: NumberLong(cursorId + i), history: history}));

        assert.commandWorked(
            mongotConn.adminCommand({setMockResponses: 1, cursorId: cursorId, history: history}));
    }
    const pipeline = [
        {
            $lookup: {
                "from": coll.getName(),
                "localField": "_id",
                "foreignField": "_id",
                "as": "JoinedIDs",
                "pipeline": [
                    {"$search": searchQuery},
                    {"$limit": 5},
                    {
                        "$project": {
                            "name": 1,
                            "paginationToken": {"$meta": "searchSequenceToken"},
                            "score": {$meta: "searchScore"}
                        }
                    }
                ]
            }
        },
    ];
    let cursor = coll.aggregate(pipeline);

    const expectedResults = [
        {"_id": 1, "test": 100, "JoinedIDs": [{"_id": 1, "paginationToken": "aaaaaaa=="}]},
        {"_id": 2, "test": 100, "JoinedIDs": [{"_id": 2, "paginationToken": "bbbbbbb=="}]},
        {"_id": 3, "test": 1, "JoinedIDs": [{"_id": 3, "paginationToken": "ccccccc=="}]},
        {"_id": 4, "test": 10, "JoinedIDs": [{"_id": 4, "paginationToken": "ddddddd=="}]},
        {"_id": 5, "test": 100, "JoinedIDs": [{"_id": 5, "paginationToken": "eeeeeee=="}]},
        {"_id": 6, "test": 100, "JoinedIDs": [{"_id": 6, "paginationToken": "fffffff=="}]},
        {"_id": 7, "test": 10, "JoinedIDs": [{"_id": 7, "paginationToken": "ggggggg=="}]},
        {"_id": 8, "test": 100, "JoinedIDs": [{"_id": 8, "paginationToken": "hhhhhhh=="}]}
    ];

    assert.eq(expectedResults, cursor.toArray());
}
testPaginationInLookup(mongotConnSBE, dbSBE, collSBE);
MongoRunner.stopMongod(connSBE);
mongotmockSBE.stop();

const mongotmockClassic = new MongotMock();
mongotmockClassic.start();
const mongotConnClassic = mongotmockClassic.getConnection();

const connClassic = MongoRunner.runMongod(
    {setParameter: {mongotHost: mongotConnClassic.host, featureFlagSearchInSbe: false}});
const dbClassic = connClassic.getDB("test");
const collClassic = dbClassic[jsTestName()];
insertDocs(collClassic);
testPaginationInLookup(mongotConnClassic, dbClassic, collClassic);
MongoRunner.stopMongod(connClassic);
mongotmockClassic.stop();
