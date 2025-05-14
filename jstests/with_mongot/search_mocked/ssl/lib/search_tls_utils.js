
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    CA_CERT,
    CLIENT_CERT,
} from "jstests/ssl/libs/ssl_helpers.js";
import {MongotMock} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

function setUpMongotAndMongodWithTLSOptions(
    {mongotMockTLSMode, mongodTLSMode = "disabled", searchTLSMode = null}) {
    const mongotmock = new MongotMock();
    mongotmock.start({bypassAuth: false, tlsMode: mongotMockTLSMode});
    const mongotConn = mongotmock.getConnection();

    let opts = {
        tlsMode: mongodTLSMode,
        setParameter: {mongotHost: mongotConn.host},
    };

    if (searchTLSMode) {
        opts.setParameter.searchTLSMode = searchTLSMode;
    }

    if (mongodTLSMode != "disabled") {
        opts.tlsCertificateKeyFile = CLIENT_CERT;
        opts.tlsCAFile = CA_CERT;
    }
    const mongodConn = MongoRunner.runMongod(opts);

    return [mongotmock, mongodConn];
}

export function verifyTLSConfigurationPasses({mongotMockTLSMode, mongodTLSMode, searchTLSMode}) {
    var [mongotmock, mongodConn] =
        setUpMongotAndMongodWithTLSOptions({mongotMockTLSMode, mongodTLSMode, searchTLSMode});
    const mongotConn = mongotmock.getConnection();

    const db = mongodConn.getDB("test");
    const coll = db.search;
    coll.drop();

    assert.commandWorked(coll.insert({"_id": 1, "title": "cakes"}));
    assert.commandWorked(coll.insert({"_id": 2, "title": "cookies and cakes"}));
    assert.commandWorked(coll.insert({"_id": 3, "title": "vegetables"}));
    assert.commandWorked(coll.insert({"_id": 4, "title": "oranges"}));
    assert.commandWorked(coll.insert({"_id": 5, "title": "cakes and oranges"}));
    assert.commandWorked(coll.insert({"_id": 6, "title": "cakes and apples"}));
    assert.commandWorked(coll.insert({"_id": 7, "title": "apples"}));
    assert.commandWorked(coll.insert({"_id": 8, "title": "cakes and kale"}));

    const collUUID = getUUIDFromListCollections(db, coll.getName());
    const searchQuery = {query: "cakes", path: "title"};
    const searchCmd =
        {search: coll.getName(), collectionUUID: collUUID, query: searchQuery, $db: "test"};

    {
        const cursorId = NumberLong(123);
        const history = [
            {
                expectedCommand: searchCmd,
                response: {
                    cursor: {
                        id: cursorId,
                        ns: coll.getFullName(),
                        nextBatch: [
                            {_id: 1, $searchScore: 0.321},
                            {_id: 2, $searchScore: 0.654},
                            {_id: 5, $searchScore: 0.789}
                        ]
                    },
                    ok: 1
                }
            },
            {
                expectedCommand: {getMore: cursorId, collection: coll.getName()},
                response: {
                    cursor: {
                        id: cursorId,
                        ns: coll.getFullName(),
                        nextBatch: [{_id: 6, $searchScore: 0.123}]
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
                        nextBatch: [{_id: 8, $searchScore: 0.345}]
                    },
                }
            },
        ];

        assert.commandWorked(
            mongotConn.adminCommand({setMockResponses: 1, cursorId: cursorId, history: history}));
    }

    // Perform a $search query.
    let cursor = coll.aggregate([{$search: searchQuery}], {cursor: {batchSize: 2}});

    const expected = [
        {"_id": 1, "title": "cakes"},
        {"_id": 2, "title": "cookies and cakes"},
        {"_id": 5, "title": "cakes and oranges"},
        {"_id": 6, "title": "cakes and apples"},
        {"_id": 8, "title": "cakes and kale"}
    ];
    assert.eq(expected, cursor.toArray());
    MongoRunner.stopMongod(mongodConn);
    mongotmock.stop();
}

export function verifyTLSConfigurationFails({
    mongotMockTLSMode,
    mongodTLSMode,
    searchTLSMode,
}) {
    var [mongotmock, mongodConn] =
        setUpMongotAndMongodWithTLSOptions({mongotMockTLSMode, mongodTLSMode, searchTLSMode});

    const db = mongodConn.getDB("test");
    const coll = db.search;
    coll.drop();

    // Add documents so mongod will try and query mongot.
    assert.commandWorked(coll.insert({"_id": 1, "title": "cakes"}));
    assert.commandWorked(coll.insert({"_id": 2, "title": "cookies and cakes"}));
    assert.commandWorked(coll.insert({"_id": 3, "title": "vegetables"}));
    const searchQuery = {query: "cakes", path: "title"};

    // Perform a $search query. It should fail with 'HostUnreachable' since the TLS mode of mongod
    // doesn't match what mongot expects.
    assert.commandFailedWithCode(
        db.runCommand({aggregate: 'search', pipeline: [{$search: searchQuery}], cursor: {}}),
        ErrorCodes.HostUnreachable);

    MongoRunner.stopMongod(mongodConn);
    mongotmock.stop();
}
