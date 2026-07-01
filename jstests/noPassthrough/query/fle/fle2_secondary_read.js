/**
 * A Queryable Encryption read against a replica set secondary should succeed
 * @tags: [requires_replication, requires_fcv_82]
 */
import {EncryptedClient, isEnterpriseShell} from "jstests/fle2/libs/encrypted_client_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

if (!isEnterpriseShell()) {
    jsTestLog("Skipping test as it requires the enterprise module");
    quit();
}

describe("FLE2 encrypted reads against a replica set secondary", function () {
    const dbName = jsTestName();
    const collName = "patients";
    const docs = [
        {_id: 0, first: "Frank", ssn: "111-11-1111"},
        {_id: 1, first: "Jane", ssn: "222-22-2222"},
        {_id: 2, first: "Frank", ssn: "333-33-3333"},
    ];

    before(function () {
        this.rst = new ReplSetTest({nodes: 2});
        this.rst.startSet();
        this.rst.initiate();

        // A replica-set-aware connection so readPreference drives server selection. Setup runs
        // with the default primary read preference.
        this.conn = new Mongo(this.rst.getURL());
        const client = new EncryptedClient(this.conn, dbName);

        // "first" is equality-queryable, so a find on it exercises the FLE tag-rewrite path.
        assert.commandWorked(
            client.createEncryptionCollection(collName, {
                encryptedFields: {
                    fields: [
                        {path: "first", bsonType: "string", queries: {queryType: "equality"}},
                        {path: "ssn", bsonType: "string"},
                    ],
                },
            }),
        );
        this.edb = client.getDB();
        docs.forEach((doc) => assert.commandWorked(this.edb[collName].einsert(doc)));
        this.rst.awaitReplication();

        // Direct subsequent reads to the secondary.
        this.conn.setReadPref("secondary");
    });

    after(function () {
        this.rst.stopSet();
    });

    it("does not error and returns correct decrypted results for an equality match", function () {
        // readPreference "secondary" excludes the primary, so these reads are served by the
        // secondary; they previously failed there with NotPrimaryNoSecondaryOk on pre-8.2 versions.
        const coll = this.edb[collName];
        assert.eq(2, coll.ecount({first: "Frank"}), "expected two docs with first == 'Frank'");
        assert.eq(
            "222-22-2222",
            coll.efindOne({first: "Jane"}).ssn,
            "ssn did not decrypt correctly",
        );
        assert.eq(0, coll.ecount({first: "Nobody"}), "expected no match");
    });
});
