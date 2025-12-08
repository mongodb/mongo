/**
 * Tests that that extension stages are generally not supported with FLE.
 * @tags: [featureFlagExtensionsAPI, requires_fcv_70]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {EncryptedClient, isEnterpriseShell} from "jstests/fle2/libs/encrypted_client_util.js";
import {checkPlatformCompatibleWithExtensions, withExtensions} from "jstests/noPassthrough/libs/extension_helpers.js";

if (!isEnterpriseShell()) {
    jsTestLog("Skipping test as it requires the enterprise module");
    quit();
}

const aggDocs = [
    {_id: 0, ssn: "123", name: "A", manager: "B", age: NumberLong(25), location: [0, 0]},
    {_id: 1, ssn: "456", name: "B", manager: "C", age: NumberLong(35), location: [0, 1]},
    {_id: 2, ssn: "789", name: "C", manager: "D", age: NumberLong(45), location: [0, 2]},
    {_id: 3, ssn: "123", name: "D", manager: "A", age: NumberLong(55), location: [0, 3]},
];

const schema = {
    encryptedFields: {
        fields: [
            {path: "ssn", bsonType: "string", queries: {queryType: "equality"}},
            {path: "age", bsonType: "long", queries: {queryType: "equality"}},
        ],
    },
};

checkPlatformCompatibleWithExtensions();

// Set up the encrypted collection.
const dbName = jsTestName();
const collName = jsTestName();

function performTest(primaryConn) {
    let db = primaryConn.getDB(dbName);
    db.dropDatabase();

    let client = new EncryptedClient(primaryConn, dbName);
    assert.commandWorked(client.createEncryptionCollection(collName, schema));
    let edb = client.getDB();

    const encryptedColl = edb[collName];
    for (const doc of aggDocs) {
        assert.commandWorked(encryptedColl.einsert(doc));
    }

    // Query analysis fails with unrecognized stage error. This is expected, because query analysis
    // does not load extensions, so it is unaware of any extensions which are not natively built
    // into the server.
    client.runEncryptionOperation(() => {
        let error = assert.throws(() => encryptedColl.aggregate([{$metrics: {}}]));
        assert.commandFailedWithCode(error, 40324);
    });

    // Verify that we can run the extension stage on the encrypted collection without
    // auto encryption. This is a sanity check, because our expected failure results from an
    // unrecognized stage being detected during parsing, which could happen if we failed to load the
    // extension.
    assertArrayEq({
        actual: encryptedColl
            .aggregate([{$metrics: {}}, {$project: {_id: 1}}, {$sort: {_id: 1}}, {$limit: 1}])
            .toArray(),
        expected: [{_id: 0}],
    });
}

// Run tests against a replica set. Running against standalone is not supported with FLE.
withExtensions({"libmetrics_mongo_extension.so": {}}, performTest, ["replica_set"]);
