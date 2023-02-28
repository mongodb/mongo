load("jstests/libs/uuid_util.js");  // for 'extractUUIDFromObject'

/*
 * Utilities for testing validation within the analyzeShardKey command and configureQueryAnalyzer
 * command. Defines test cases for namespace, shard key and index validation.
 */
function ValidationTest(conn) {
    const dbName = "testDb-" + extractUUIDFromObject(UUID());
    const db = conn.getDB(dbName);

    // Create a regular collection.
    const testBasicCollName = "testBasicColl";
    assert.commandWorked(db.createCollection(testBasicCollName));

    // Create an FLE collection.
    const testFLECollName = "testFLEColl";
    const sampleEncryptedFields = {
        "fields": [
            {
                "path": "firstName",
                "keyId": UUID("11d58b8a-0c6c-4d69-a0bd-70c6d9befae9"),
                "bsonType": "string",
                "queries": {"queryType": "equality"}  // allow single object or array
            },
        ]
    };
    assert.commandWorked(
        db.createCollection(testFLECollName, {encryptedFields: sampleEncryptedFields}));

    // Create a view.
    const testViewName = "testView";
    assert.commandWorked(
        db.runCommand({create: testViewName, viewOn: testBasicCollName, pipeline: []}));

    // Make the regular collection the default test collection.
    const collName = testBasicCollName;

    /*
     * Returns 'numDocs' documents with the same set of fields where the value of each field in
     * each document is distinct.
     */
    function makeDocuments(numDocs) {
        const docs = [];
        for (let i = 0; i < numDocs; i++) {
            docs.push({a: i, b: [i * 0.001, i * 0.001], c: i});
        }
        return {docs, arrayFieldName: "b"};
    }

    // Define validation test cases.
    const invalidNamespaceTestCases = [
        // Cannot analyze a shard key or queries for a collection is the config database.
        {
            // On a sharded cluster, this has the config server as the primary shard.
            dbName: "config",
            collName: "chunks"
        },
        {
            // On a sharded cluster, this is a sharded collection.
            dbName: "config",
            collName: "system.sessions"
        },
        // Cannot analyze a shard key or queries for a collection is the local database.
        {dbName: "local", collName: "rs.oplog"},
        // Cannot analyze a shard key or queries for a collection is the admin database.
        {dbName: "admin", collName: "users"},
        // Cannot analyze a shard key or queries for a system collection.
        {dbName: dbName, collName: "system.profile"},
        // Cannot analyze a shard key or queries for an FLE state collection.
        {dbName: dbName, collName: "enxcol_.basic.esc"},
        {dbName: dbName, collName: "enxcol_.basic.ecc"},
        {dbName: dbName, collName: "enxcol_.basic.ecoc"},
        // Cannot analyze a shard key or queries for a collection with FLE enabled.
        {dbName: dbName, collName: "testFLEColl"},
        {dbName: dbName, collName: testViewName, isView: true}
    ];

    return {
        dbName,
        collName,
        makeDocuments,
        invalidNamespaceTestCases,
    };
}
