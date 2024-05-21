/**
 * Tests basic CRUD operations with queryable encrypted fields.
 *
 * @tags: [
 *   no_selinux,
 *   tenant_migration_incompatible,
 *   does_not_support_transactions,
 *   does_not_support_stepdowns,
 *   # Test requires an internal connection for the keyvault that can't be overriden by the
 *   # `simulate_atlas_proxy` override.
 *   simulate_atlas_proxy_incompatible,
 * ]
 */
import {
    assertIsIndexedEncryptedField,
    EncryptedClient,
    kSafeContentField
} from "jstests/fle2/libs/encrypted_client_util.js";

const buildInfo = assert.commandWorked(db.runCommand({"buildInfo": 1}));

if (!(buildInfo.modules.includes("enterprise"))) {
    jsTestLog("Skipping test as it requires the enterprise module");
    quit();
}

const dbName = "qetestdb";
const collName = "qetestcoll";
const initialConn = db.getMongo();
const localKMS = {
    key: BinData(
        0,
        "/tu9jUCBqZdwCelwE/EAm/4WqdxrSMi04B8e9uAV+m30rI1J2nhKZZtQjdvsSCwuI4erR6IEcEK+5eGUAODv43NDNIR9QheT2edWFewUfHKsl9cnzTc86meIzOmYl6dr")
};

// Some tests silently change the DB name to prefix it with a tenant ID, but we
// need to pass the real DB name for the keyvault when setting up the auto encryption,
// so that the internal connection for the key vault will target the right DB name.
const kvDbName = (typeof (initialConn.getDbNameWithTenantPrefix) === "function")
    ? initialConn.getDbNameWithTenantPrefix(dbName)
    : dbName;
jsTestLog("Using key vault db " + kvDbName);

const clientSideFLEOptions = {
    kmsProviders: {local: localKMS},
    keyVaultNamespace: kvDbName + ".keystore",
    schemaMap: {},
};

db.getSiblingDB(dbName).dropDatabase();

assert(initialConn.setAutoEncryption(clientSideFLEOptions));

let encryptedClient = new EncryptedClient(initialConn, dbName);
assert.commandWorked(encryptedClient.createEncryptionCollection(collName, {
    encryptedFields: {
        "fields": [
            {"path": "first", "bsonType": "string", "queries": {"queryType": "equality"}},
        ]
    }
}));

initialConn.toggleAutoEncryption(true);

function runIndexedEqualityEncryptedCRUDTest(client, iterations) {
    let conn = client.getDB().getMongo();
    let ecoll = client.getDB()[collName];
    let values =
        [["frodo", "baggins"], ["sam", "gamgee"], ["pippin", "took"], ["merry", "brandybuck"]];
    let count = 0;
    let escCount = 0;
    let ecocCount = 0;

    // Do encrypted inserts
    for (let it = 0; it < iterations; it++) {
        for (let val of values) {
            assert.commandWorked(ecoll.insert({_id: count, first: val[0], last: val[1]}));
            count++;
            client.assertEncryptedCollectionCounts(collName, count, count, count);
        }
    }
    escCount = count;
    ecocCount = count;

    // Do finds using unencrypted connection
    {
        conn.toggleAutoEncryption(false);

        let rawDocs = ecoll.find().toArray();
        assert.eq(rawDocs.length, count);
        for (let rawDoc of rawDocs) {
            assertIsIndexedEncryptedField(rawDoc.first);
            assert(rawDoc[kSafeContentField] !== undefined);
        }
        conn.toggleAutoEncryption(true);
    }

    // Do encrypted queries using encrypted connection
    for (let mod = 0; mod < values.length; mod++) {
        let docs = ecoll.find({last: values[mod][1]}).toArray();

        for (let doc of docs) {
            assert.eq(doc._id % values.length, mod);
            assert.eq(doc.first, values[mod][0]);
            assert(doc[kSafeContentField] !== undefined);
        }
    }

    // Do updates on encrypted fields
    for (let it = 0; it < iterations; it++) {
        let res = assert.commandWorked(ecoll.updateOne(
            {$and: [{last: "baggins"}, {first: "frodo"}]}, {$set: {first: "bilbo"}}));
        assert.eq(res.matchedCount, 1);
        assert.eq(res.modifiedCount, 1);
        escCount++;
        ecocCount++;
        client.assertEncryptedCollectionCounts(collName, count, escCount, ecocCount);

        res = assert.commandWorked(
            ecoll.replaceOne({last: "took"}, {first: "paladin", last: "took"}));
        assert.eq(res.matchedCount, 1);
        assert.eq(res.modifiedCount, 1);
        escCount++;
        ecocCount++;
        client.assertEncryptedCollectionCounts(collName, count, escCount, ecocCount);
    }

    // Do findAndModifies
    for (let it = 0; it < iterations; it++) {
        let res = assert.commandWorked(ecoll.runCommand({
            findAndModify: ecoll.getName(),
            query: {$and: [{last: "gamgee"}, {first: "sam"}]},
            update: {$set: {first: "rosie"}},
        }));
        print(tojson(res));
        assert.eq(res.value.first, "sam");
        assert(res.value[kSafeContentField] !== undefined);
        escCount++;
        ecocCount++;
        client.assertEncryptedCollectionCounts(collName, count, escCount, ecocCount);
    }

    // Do deletes
    for (let it = 0; it < iterations; it++) {
        let res = assert.commandWorked(
            ecoll.deleteOne({last: "brandybuck"}, {writeConcern: {w: "majority"}}));
        assert.eq(res.deletedCount, 1);
        count--;
        client.assertEncryptedCollectionCounts(collName, count, escCount, ecocCount);
    }
    assert.eq(ecoll.find({last: "brandybuck"}).count(), 0);
}

// Test CRUD on indexed equality encrypted fields
runIndexedEqualityEncryptedCRUDTest(encryptedClient, 10);

encryptedClient = undefined;
initialConn.unsetAutoEncryption();
