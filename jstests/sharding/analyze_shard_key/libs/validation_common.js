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
    const listCollectionRes = assert.commandWorked(
        db.runCommand({listCollections: 1, filter: {name: testBasicCollName}}));
    const isClusteredColl =
        listCollectionRes.cursor.firstBatch[0].options.hasOwnProperty("clusteredIndex");

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
    const invalidShardKeyTestCases = [
        // Cannot analyze an empty shard key.
        {},
        // Cannot analyze an shard key with an empty field name.
        {"": 1},
        // Cannot analyze a shard key with a field that is neither "hashed" or 1.
        {a: "2d"},
        {a: "2dsphere"},
        {a: "columnstore"},
        {a: 0},
        // Cannot analyze a shard key with more than one "hashed" field.
        {a: "hashed", b: "hashed"},
        // Cannot analyze a shard key with a field that contains an extra dot at the end.
        {"a.": 1},
        // Cannot analyze a shard key with a field that contains two consecutive dots.
        {"a..": 1},
        // Cannot analyze a shard key with a field that contains parts that start with '$'.
        {"$a": 1},
        {"a.$x": 1},
        {"$**": 1},
        {"a.$**": 1},
    ];
    // The analyzeShardKey command cannot use the index below for calculating cardinality and
    // frequency metrics.
    const noCompatibleIndexTestCases = [
        {indexOptions: {key: {b: "2d"}, name: "b_2d"}, shardKey: {b: 1}},
        {indexOptions: {key: {b: "2dsphere"}, name: "b_2dsphere"}, shardKey: {b: 1}},
        {indexOptions: {key: {a: "text"}, name: "a_text"}, shardKey: {a: 1}},
        {indexOptions: {key: {"$**": 1}, name: "wildcard"}, shardKey: {a: 1}},
        {
            indexOptions: {key: {"a": 1}, name: "a_sparse", sparse: true},
            shardKey: {a: 1},
        },
        {
            indexOptions:
                {key: {"a": 1}, name: "a_partial", partialFilterExpression: {c: {$gt: 5}}},
            shardKey: {a: 1},
        },
        {
            indexOptions: {
                key: {"a": 1},
                name: "a_non_simple_collation",
                collation: {locale: "en_US", strength: 1, caseLevel: false}
            },
            shardKey: {a: 1},
        },
    ];
    // Note that clustered collections do not support columnstore indexes.
    if (!isClusteredColl) {
        noCompatibleIndexTestCases.push(
            {indexOptions: {key: {"$**": "columnstore"}, name: "columnstore"}, shardKey: {a: 1}});
    }

    return {
        dbName,
        collName,
        makeDocuments,
        invalidNamespaceTestCases,
        invalidShardKeyTestCases,
        noCompatibleIndexTestCases
    };
}
