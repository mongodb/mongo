/**
 * Test that a client must not be able to override the trusted, mongod-owned fields in the command
 * that mongod constructs and sends to mongot for a $vectorSearch query.
 *
 * mongod adds certain fields to the mongot command from trusted sources (the resolved collection
 * name, the collection UUID, and the authorized view name) and then merges the raw user
 * $vectorSearch spec on top for passthrough. This process should not allow the user to override the
 * trusted fields.
 *
 * The 'explain' field is only rejected during an explained aggregate where mongod builds a command
 * with an 'explain' field containing the verbosity, which would conflict. This is the only scenario
 * where it might cause a problem/ambiguity, so we only restrict/error on explains.
 *
 * @tags: [
 * ]
 */
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    mongotCommandForVectorSearchQuery,
    MongotMock,
    mongotResponseForBatch,
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

const kTrustedFieldErrorCode = 12961800;
const kExplainConflictErrorCode = 10804601;

const dbName = jsTestName();
const collName = jsTestName();

const baseQuery = {
    queryVector: [1.0, 2.0, 3.0],
    path: "embedding",
    numCandidates: 10,
    limit: 5,
    index: "vector_index",
};

// Set up mongotmock and point a mongod at it.
const mongotmock = new MongotMock();
mongotmock.start();
const conn = MongoRunner.runMongod({
    setParameter: {mongotHost: mongotmock.getConnection().host},
});

const testDB = conn.getDB(dbName);
const coll = testDB.getCollection(collName);
assert.commandWorked(coll.insert({_id: 1, embedding: [1.0, 2.0, 3.0]}));
const collectionUUID = getUUIDFromListCollections(testDB, collName);

// Each injected field must cause the aggregate to fail before mongod contacts mongot.
function assertInjectedFieldIsRejected(injectedFields) {
    assert.commandFailedWithCode(
        coll.getDB().runCommand({
            aggregate: coll.getName(),
            pipeline: [{$vectorSearch: {...baseQuery, ...injectedFields}}],
            cursor: {},
        }),
        kTrustedFieldErrorCode,
        "injecting a trusted field should be rejected",
        {injectedFields},
    );
    // The failure happens in mongod before contacting mongot, so nothing should be sent.
    mongotmock.assertEmpty();
}

assertInjectedFieldIsRejected({viewName: "someView"});
assertInjectedFieldIsRejected({collectionUUID: UUID()});

// Even a valid UUID for the very collection being queried must be rejected: the field name
// itself is reserved, regardless of its value.
assertInjectedFieldIsRejected({collectionUUID: collectionUUID});

assertInjectedFieldIsRejected({vectorSearch: "someCollection"});

// 'explain' is not a trusted field and is allowed in non-explained aggregates, where it passes
// through to mongot untouched (it may become a valid $vectorSearch field in the future).
{
    const cursorId = NumberLong(1);
    const userExplain = {verbosity: "queryPlanner"};

    const expectedCommand = mongotCommandForVectorSearchQuery({
        ...baseQuery,
        explain: userExplain,
        collName: coll.getName(),
        dbName,
        collectionUUID,
    });
    const response = mongotResponseForBatch(
        [{_id: 1, $vectorSearchScore: 0.9}],
        NumberLong(0),
        `${dbName}.${collName}`,
        1,
    );

    mongotmock.setMockResponses([{expectedCommand, response}], cursorId);

    assert.commandWorked(
        coll.getDB().runCommand({
            aggregate: coll.getName(),
            pipeline: [{$vectorSearch: {...baseQuery, explain: userExplain}}],
            cursor: {},
        }),
    );

    mongotmock.assertEmpty();
}

// 'explain' is allowed at parse time, but conflicts with the mongod-owned 'explain' field when
// the aggregate is explained, so it is rejected while building the command for mongot.
assert.commandFailedWithCode(
    coll.getDB().runCommand({
        explain: {
            aggregate: coll.getName(),
            pipeline: [
                {$vectorSearch: {...baseQuery, explain: {verbosity: "queryPlanner"}}},
            ],
            cursor: {},
        },
        verbosity: "queryPlanner",
    }),
    kExplainConflictErrorCode,
    "a user-supplied explain field should be rejected when explaining",
);
// The failure happens in mongod while building the command, before contacting mongot.
mongotmock.assertEmpty();

MongoRunner.stopMongod(conn);
mongotmock.stop();
