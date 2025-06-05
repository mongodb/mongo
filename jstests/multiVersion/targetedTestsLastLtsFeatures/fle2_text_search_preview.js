/**
 * Test downgrade to incompatible versions is blocked if substringPreview,
 * suffixPreview, or prefixPreview query types are being used in a FLE2 collection.
 */
import "jstests/multiVersion/libs/multi_rs.js";

import {EncryptedClient} from "jstests/fle2/libs/encrypted_client_util.js";
import {
    PrefixField,
    SubstringField,
    SuffixAndPrefixField,
    SuffixField
} from "jstests/fle2/libs/qe_text_search_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const dbName = 'qe_text_downgrade_test';
const substrField = new SubstringField(20, 2, 10, false, false, 1);
const suffixField = new SuffixField(2, 5, true, false, 1);
const prefixField = new PrefixField(2, 5, false, true, 1);
const comboField = new SuffixAndPrefixField(2, 5, 2, 5, false, false, 1);

function testBinaryDowngrade(queryTypeConfig) {
    jsTestLog("Testing downgrade from latest to last-lts");
    const rst = new ReplSetTest(
        {nodes: 2, nodeOptions: {setParameter: {featureFlagQETextSearchPreview: true}}});
    rst.startSet();
    rst.initiate();

    const conn = rst.getPrimary();
    const client = new EncryptedClient(conn, dbName);
    const edb = client.getDB();
    const adminDB = conn.getDB('admin');

    assert.commandWorked(client.createEncryptionCollection("basic_equality_and_range", {
        encryptedFields: {
            "fields": [
                {path: "first", bsonType: "string", queries: {queryType: "equality"}},
                {
                    path: "second",
                    bsonType: "int",
                    queries: {queryType: "range", sparsity: 1, trimFactor: 0}
                }
            ]
        }
    }));

    assert.commandWorked(client.createEncryptionCollection("basic_text", {
        encryptedFields: {
            "fields": [{
                path: "first",
                bsonType: "string",
                queries: queryTypeConfig.createQueryTypeDescriptor()
            }]
        }
    }));

    // Downgrade should fail because of basic_text
    assert.commandFailedWithCode(
        adminDB.runCommand(
            {setFeatureCompatibilityVersion: lastLTSFCV, confirm: true, writeConcern: {w: 1}}),
        ErrorCodes.CannotDowngrade);

    // Drop basic_text
    edb.basic_text.drop();

    assert.commandWorked(adminDB.runCommand(
        {setFeatureCompatibilityVersion: lastLTSFCV, confirm: true, writeConcern: {w: 1}}));

    // Downgrade should now succeed
    jsTestLog("Starting binary downgrade to last LTS");
    rst.upgradeSet({binVersion: 'last-lts', setParameter: {}});
    jsTestLog("Finished binary downgrade to last LTS");

    rst.stopSet();
}

testBinaryDowngrade(substrField);
testBinaryDowngrade(suffixField);
testBinaryDowngrade(prefixField);
testBinaryDowngrade(comboField);
