/**
 * Check that mongod can use the password protected intracluster certificates for
 * communication with mongot.
 */
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {CA_CERT, SERVER_CERT, CLIENT_PASSWORD_PROTECTED_CERT} from "jstests/ssl/libs/ssl_helpers.js";
import {MongotMock} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

function runOneTest(mongodTlsOpts, badPassword) {
    // Set up mongotmock and point the mongod to it.
    const mongotmock = new MongotMock();
    mongotmock.start({tlsMode: "requireTLS"});
    const mongotConn = mongotmock.getConnection();

    const opts = Object.assign(
        {
            sslMode: "requireSSL",
            tlsAllowInvalidCertificates: "",
            setParameter: {mongotHost: mongotConn.host, searchTLSMode: "requireTLS"},
        },
        mongodTlsOpts,
    );

    if (badPassword) {
        assert.throws(() => MongoRunner.runMongod(opts));
        mongotmock.stop();
        return;
    }

    const conn = MongoRunner.runMongod(opts);

    const db = conn.getDB("test");
    const collName = "search";
    const coll = db.getCollection(collName);
    const searchQuery = {
        query: "cakes",
        path: "title",
    };
    const pipeline = [{$search: searchQuery}];
    coll.drop();
    assert.commandWorked(coll.insert({"_id": 1, "title": "cakes"}));

    const collUUID = getUUIDFromListCollections(db, collName);
    // Give mongotmock some stuff to return.
    {
        const cursorId = NumberLong(123);
        const searchCmd = {search: collName, collectionUUID: collUUID, query: searchQuery, $db: "test"};
        const history = [
            {
                expectedCommand: searchCmd,
                response: {
                    cursor: {
                        id: NumberLong(0),
                        ns: "test." + collName,
                        nextBatch: [{_id: 1, $searchScore: 0.321}],
                    },
                    ok: 1,
                },
            },
        ];

        assert.commandWorked(mongotConn.adminCommand({setMockResponses: 1, cursorId: cursorId, history: history}));
    }

    // Perform a $search query.
    let cursor = db[collName].aggregate(pipeline);

    const expected = [{"_id": 1, "title": "cakes"}];
    assert.eq(expected, cursor.toArray());

    MongoRunner.stopMongod(conn);
    mongotmock.stop();
}

const optsClusterFile = {
    sslCAFile: CA_CERT,
    sslPEMKeyFile: SERVER_CERT,
    tlsClusterCAFile: CA_CERT,
    tlsClusterFile: CLIENT_PASSWORD_PROTECTED_CERT,
};
// Test usage of password protected cluster file
runOneTest({...optsClusterFile, tlsClusterPassword: "foobar"}, true);
runOneTest({...optsClusterFile, tlsClusterPassword: "qwerty"}, false);

const optsPEMKeyFile = {
    sslCAFile: CA_CERT,
    sslPEMKeyFile: CLIENT_PASSWORD_PROTECTED_CERT,
};

// Test usage of password protected PEM key file
runOneTest({...optsPEMKeyFile, tlsCertificateKeyFilePassword: "foobar"}, true);
runOneTest({...optsPEMKeyFile, tlsCertificateKeyFilePassword: "qwerty"}, false);
