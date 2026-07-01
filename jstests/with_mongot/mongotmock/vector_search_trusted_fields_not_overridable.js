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
import {after, before, describe, it} from "jstests/libs/mochalite.js";
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

describe("$vectorSearch trusted fields are not user-overridable", function () {
    before(function () {
        // Set up mongotmock and point a mongod at it.
        this.mongotmock = new MongotMock();
        this.mongotmock.start();
        this.conn = MongoRunner.runMongod({
            setParameter: {mongotHost: this.mongotmock.getConnection().host},
        });

        const testDB = this.conn.getDB(dbName);
        const coll = testDB.getCollection(collName);
        assert.commandWorked(coll.insert({_id: 1, embedding: [1.0, 2.0, 3.0]}));
        this.collectionUUID = getUUIDFromListCollections(testDB, collName);
        this.coll = coll;
    });

    after(function () {
        MongoRunner.stopMongod(this.conn);
        this.mongotmock.stop();
    });

    // Each injected field must cause the aggregate to fail before mongod contacts mongot.
    function assertInjectedFieldIsRejected(coll, mongotmock, injectedFields) {
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

    it("rejects an injected viewName", function () {
        assertInjectedFieldIsRejected(this.coll, this.mongotmock, {viewName: "someView"});
    });

    it("rejects an injected collectionUUID", function () {
        assertInjectedFieldIsRejected(this.coll, this.mongotmock, {collectionUUID: UUID()});
    });

    // Even a valid UUID for the very collection being queried must be rejected: the field name
    // itself is reserved, regardless of its value.
    it("rejects an injected collectionUUID that matches the real collection", function () {
        assertInjectedFieldIsRejected(this.coll, this.mongotmock, {
            collectionUUID: this.collectionUUID,
        });
    });

    it("rejects an injected vectorSearch collection name", function () {
        assertInjectedFieldIsRejected(this.coll, this.mongotmock, {vectorSearch: "someCollection"});
    });

    // 'explain' is not a trusted field and is allowed in non-explained aggregates, where it passes
    // through to mongot untouched (it may become a valid $vectorSearch field in the future).
    it("allows a user-supplied explain field when the aggregate is not explained", function () {
        const cursorId = NumberLong(1);
        const userExplain = {verbosity: "queryPlanner"};

        const expectedCommand = mongotCommandForVectorSearchQuery({
            ...baseQuery,
            explain: userExplain,
            collName: this.coll.getName(),
            dbName,
            collectionUUID: this.collectionUUID,
        });
        const response = mongotResponseForBatch(
            [{_id: 1, $vectorSearchScore: 0.9}],
            NumberLong(0),
            `${dbName}.${collName}`,
            1,
        );

        this.mongotmock.setMockResponses([{expectedCommand, response}], cursorId);

        assert.commandWorked(
            this.coll.getDB().runCommand({
                aggregate: this.coll.getName(),
                pipeline: [{$vectorSearch: {...baseQuery, explain: userExplain}}],
                cursor: {},
            }),
        );

        this.mongotmock.assertEmpty();
    });

    // 'explain' is allowed at parse time, but conflicts with the mongod-owned 'explain' field when
    // the aggregate is explained, so it is rejected while building the command for mongot.
    it("rejects a user-supplied explain field when the aggregate is explained", function () {
        assert.commandFailedWithCode(
            this.coll.getDB().runCommand({
                explain: {
                    aggregate: this.coll.getName(),
                    pipeline: [{$vectorSearch: {...baseQuery, explain: {verbosity: "queryPlanner"}}}],
                    cursor: {},
                },
                verbosity: "queryPlanner",
            }),
            kExplainConflictErrorCode,
            "a user-supplied explain field should be rejected when explaining",
        );
        // The failure happens in mongod while building the command, before contacting mongot.
        this.mongotmock.assertEmpty();
    });
});
