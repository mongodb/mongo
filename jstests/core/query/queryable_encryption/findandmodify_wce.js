/**
 * Tests FLE2 findAndModify reports a WriteConcernError from internal transaction
 * in spite of command error.
 *
 * @tags: [
 *   no_selinux,
 *   does_not_support_transactions,
 *   does_not_support_stepdowns,
 *   # Test requires an internal connection for the keyvault that can't be overriden by the
 *   # `simulate_atlas_proxy` override.
 *   simulate_atlas_proxy_incompatible,
 *   assumes_write_concern_unchanged,
 *   requires_fcv_81,
 *   # TODO (SERVER-102377): Re-enable this test.
 *   DISABLED_TEMPORARILY_DUE_TO_FCV_UPGRADE
 * ]
 */
import {
    EncryptedClient,
} from "jstests/fle2/libs/encrypted_client_util.js";

const buildInfo = assert.commandWorked(db.runCommand({"buildInfo": 1}));

if (!(buildInfo.modules.includes("enterprise"))) {
    jsTestLog("Skipping test as it requires the enterprise module");
    quit();
}

const dbName = "qetestdb_fam_wce";
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

function runTest(client) {
    const ecoll = client.getDB()[collName];

    // Do an encrypted insert
    assert.commandWorked(ecoll.insert({_id: 1, first: "frodo", last: "baggins"}));
    client.assertEncryptedCollectionCounts(collName, 1, 1, 1);

    // Do a findAndModify with unsatisfiable WCE.
    const UNSATISFIABLE_WC = {
        w: 50,
        j: false,
        wtimeout: 1000,
    };
    const cmdModifyId = {
        findAndModify: collName,
        query: {first: "frodo"},
        update: {$set: {_id: 2}},
        writeConcern: UNSATISFIABLE_WC
    };
    let res = assert.commandFailedWithCode(client.getDB().runCommand(cmdModifyId),
                                           ErrorCodes.ImmutableField);
    print("result: " + tojson(res));
    assert(res.hasOwnProperty("writeConcernError"));
    assert.eq(res.writeConcernError.code, ErrorCodes.UnsatisfiableWriteConcern);

    const cmdExplain = {explain: cmdModifyId};
    res = assert.commandWorked(client.getDB().runCommand(cmdExplain));
    print("result: " + tojson(res));
}

runTest(encryptedClient);

encryptedClient = undefined;
initialConn.unsetAutoEncryption();
