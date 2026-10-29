/**
 * Tests that query stats on write commands are NOT collected for FLE (Field Level Encryption)
 * operations. A command carrying encryptionInformation signals an FLE operation and is excluded
 * from query stats collection.
 */
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {
    getQueryStatsDeleteCmd,
    getQueryStatsInsertCmd,
    getQueryStatsUpdateCmd,
    resetQueryStatsStore,
} from "jstests/libs/query/query_stats_utils.js";

const collName = jsTestName();

describe("FLE query stats not collected", function () {
    let rst;
    let conn;
    let testDB;
    let escCollection;
    let ecocCollection;

    before(function () {
        rst = new ReplSetTest({
            nodes: 1,
            nodeOptions: {
                setParameter: {
                    internalQueryStatsSampleRate: 1,
                    internalQueryStatsWriteCmdSampleRate: 1,
                },
            },
        });
        rst.startSet();
        rst.initiate();
        conn = rst.getPrimary();
        testDB = conn.getDB("test");

        testDB[collName].drop();
        assert.commandWorked(
            testDB.createCollection(collName, {
                encryptedFields: {
                    fields: [
                        {
                            path: "secret",
                            keyId: UUID("11d58b8a-0c6c-4d69-a0bd-70c6d9befae9"),
                            bsonType: "string",
                            queries: {queryType: "equality"},
                        },
                    ],
                },
            }),
        );
        const collInfo = testDB.getCollectionInfos({name: collName})[0];
        escCollection = collInfo.options.encryptedFields.escCollection;
        ecocCollection = collInfo.options.encryptedFields.ecocCollection;
    });

    after(function () {
        rst?.stopSet();
    });

    beforeEach(function () {
        resetQueryStatsStore(conn, "1MB");
    });

    // Returns the encryptionInformation payload that marks a command as an FLE operation.
    function buildEncryptionInfo() {
        return {
            type: 1,
            schema: {
                ["test." + collName]: {
                    escCollection: escCollection,
                    ecocCollection: ecocCollection,
                    fields: [
                        {
                            path: "secret",
                            keyId: UUID("11d58b8a-0c6c-4d69-a0bd-70c6d9befae9"),
                            bsonType: "string",
                            queries: {queryType: "equality"},
                        },
                    ],
                },
            },
        };
    }

    it("should not record insert query stats for an insert carrying encryptionInformation", function () {
        // The FLE insert may succeed or fail depending on server configuration; only
        // the absence of query stats matters here.
        testDB.runCommand({
            insert: collName,
            documents: [{v: 1}],
            encryptionInformation: buildEncryptionInfo(),
        });
        assert.eq(
            0,
            getQueryStatsInsertCmd(conn, {collName: collName}).length,
            "Expected no insert stats for FLE insert",
        );
    });

    it("should not record delete query stats for a delete carrying encryptionInformation", function () {
        // The FLE delete may succeed or fail depending on server configuration; only
        // the absence of query stats matters here.
        testDB.runCommand({
            delete: collName,
            deletes: [{q: {v: 1}, limit: 1}],
            encryptionInformation: buildEncryptionInfo(),
        });
        assert.eq(
            0,
            getQueryStatsDeleteCmd(conn, {collName: collName}).length,
            "Expected no delete stats for FLE delete",
        );
    });

    it("should not record update query stats for an update carrying encryptionInformation", function () {
        // The FLE update may succeed or fail depending on server configuration; only
        // the absence of query stats matters here.
        testDB.runCommand({
            update: collName,
            updates: [{q: {v: 1}, u: {$set: {v: 2}}}],
            encryptionInformation: buildEncryptionInfo(),
        });
        assert.eq(
            0,
            getQueryStatsUpdateCmd(conn, {collName: collName}).length,
            "Expected no update stats for FLE update",
        );
    });
});
