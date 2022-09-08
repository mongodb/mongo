load("jstests/concurrency/fsm_workload_helpers/server_types.js");  // For isMongos.

/**
 * Create a FLE client that has an unencrypted and encrypted client to the same database
 */

var kSafeContentField = "__safeContent__";

var EncryptedClient = class {
    /**
     * Create a new encrypted FLE connection to the target server with a local KMS
     *
     * @param {Mongo} conn Connection to mongod or mongos
     * @param {string} dbName Name of database to setup key vault in
     */
    constructor(conn, dbName) {
        // Detect if jstests/libs/override_methods/implicitly_shard_accessed_collections.js is in
        // use
        this.useImplicitSharding = !(typeof (ImplicitlyShardAccessCollSettings) === "undefined");

        const localKMS = {
            key: BinData(
                0,
                "/tu9jUCBqZdwCelwE/EAm/4WqdxrSMi04B8e9uAV+m30rI1J2nhKZZtQjdvsSCwuI4erR6IEcEK+5eGUAODv43NDNIR9QheT2edWFewUfHKsl9cnzTc86meIzOmYl6dr")
        };

        const clientSideFLEOptions = {
            kmsProviders: {
                local: localKMS,
            },
            keyVaultNamespace: dbName + ".keystore",
            schemaMap: {},
        };

        let connectionString = conn.host.toString();
        var shell = Mongo(connectionString, clientSideFLEOptions);
        var edb = shell.getDB(dbName);

        var keyVault = shell.getKeyVault();

        this._db = conn.getDB(dbName);
        this._edb = edb;
        this._keyVault = keyVault;
    }

    /**
     * Return an encrypted database
     *
     * @returns Database
     */
    getDB() {
        return this._edb;
    }

    /**
     * Return an unencrypted database
     *
     * @returns Database
     */
    getRawDB() {
        return this._db;
    }

    /**
     * @returns KeyVault
     */
    getKeyVault() {
        return this._keyVault;
    }

    /**
     * Create an encrypted collection. If key ids are not specified, it creates them automatically
     * in the key vault.
     *
     * @param {string} name Name of collection
     * @param {Object} options Create Collection options
     */
    createEncryptionCollection(name, options) {
        assert(options != undefined);
        assert(options.hasOwnProperty("encryptedFields"));
        assert(options.encryptedFields.hasOwnProperty("fields"));

        for (let field of options.encryptedFields.fields) {
            if (!field.hasOwnProperty("keyId")) {
                let testkeyId = this._keyVault.createKey("local", "ignored");
                field["keyId"] = testkeyId;
            }
        }

        assert.neq(options,
                   undefined,
                   `createEncryptedCollection expected an options object, it is undefined`);
        assert(
            options.hasOwnProperty("encryptedFields") && typeof options.encryptedFields == "object",
            `options must contain an encryptedFields document'`);

        const res = assert.commandWorked(this._edb.createCollection(name, options));

        const cis = this._edb.getCollectionInfos({"name": name});
        assert.eq(cis.length, 1, `Expected to find one collection named '${name}'`);

        const ci = cis[0];
        assert(ci.hasOwnProperty("options"), `Expected collection '${name}' to have 'options'`);
        const storedOptions = ci.options;
        assert(options.hasOwnProperty("encryptedFields"),
               `Expected collection '${name}' to have 'encryptedFields'`);
        const ef = storedOptions.encryptedFields;

        // All our tests use "last" as the key to query on so shard on "last" instead of "_id"
        if (this.useImplicitSharding) {
            let resShard = this._db.adminCommand({enableSharding: this._db.getName()});

            // enableSharding may only be called once for a database.
            if (resShard.code !== ErrorCodes.AlreadyInitialized) {
                assert.commandWorked(
                    resShard, "enabling sharding on the '" + this._db.getName() + "' db failed");
            }

            let shardCollCmd = {
                shardCollection: this._db.getName() + "." + name,
                key: {last: "hashed"},
                collation: {locale: "simple"}
            };

            resShard = this._db.adminCommand(shardCollCmd);

            jsTestLog("Sharding: " + tojson(shardCollCmd));
        }

        assert.commandWorked(this._edb.getCollection(name).createIndex({__safeContent__: 1}));

        assert.commandWorked(this._edb.createCollection(ef.escCollection));
        assert.commandWorked(this._edb.createCollection(ef.eccCollection));
        assert.commandWorked(this._edb.createCollection(ef.ecocCollection));

        return res;
    }

    /**
     * Assert the number of documents in the EDC and state collections is correct.
     *
     * @param {object} collection Collection object for EDC
     * @param {number} edc Number of documents in EDC
     * @param {number} esc Number of documents in ESC
     * @param {number} ecc Number of documents in ECC
     * @param {number} ecoc Number of documents in ECOC
     */
    assertEncryptedCollectionCountsByObject(
        sessionDB, name, expectedEdc, expectedEsc, expectedEcc, expectedEcoc) {
        const cis = this._db.getCollectionInfos({"name": name});
        assert.eq(cis.length, 1, `Expected to find one collection named '${name}'`);

        const ci = cis[0];
        assert(ci.hasOwnProperty("options"), `Expected collection '${name}' to have 'options'`);
        const options = ci.options;
        assert(options.hasOwnProperty("encryptedFields"),
               `Expected collection '${name}' to have 'encryptedFields'`);

        const ef = options.encryptedFields;

        const actualEdc = sessionDB.getCollection(name).countDocuments({});
        assert.eq(actualEdc,
                  expectedEdc,
                  `EDC document count is wrong: Actual ${actualEdc} vs Expected ${expectedEdc}`);

        const actualEsc = sessionDB.getCollection(ef.escCollection).countDocuments({});
        assert.eq(actualEsc,
                  expectedEsc,
                  `ESC document count is wrong: Actual ${actualEsc} vs Expected ${expectedEsc}`);

        const actualEcc = sessionDB.getCollection(ef.eccCollection).countDocuments({});
        assert.eq(actualEcc,
                  expectedEcc,
                  `ECC document count is wrong: Actual ${actualEcc} vs Expected ${expectedEcc}`);

        const actualEcoc = sessionDB.getCollection(ef.ecocCollection).countDocuments({});
        assert.eq(actualEcoc,
                  expectedEcoc,
                  `ECOC document count is wrong: Actual ${actualEcoc} vs Expected ${expectedEcoc}`);
    }

    /**
     * Assert the number of documents in the EDC and state collections is correct.
     *
     * @param {string} name Name of EDC
     * @param {number} edc Number of documents in EDC
     * @param {number} esc Number of documents in ESC
     * @param {number} ecc Number of documents in ECC
     * @param {number} ecoc Number of documents in ECOC
     */
    assertEncryptedCollectionCounts(name, expectedEdc, expectedEsc, expectedEcc, expectedEcoc) {
        this.assertEncryptedCollectionCountsByObject(
            this._db, name, expectedEdc, expectedEsc, expectedEcc, expectedEcoc);
    }

    /**
     * Get a single document from the collection with the specified query. Ensure it contains the
     specified fields when decrypted and that does fields are encrypted.

     * @param {string} coll
     * @param {object} query
     * @param {object} fields
     */
    assertOneEncryptedDocumentFields(coll, query, fields) {
        let encryptedDocs = this._db.getCollection(coll).find(query).toArray();
        assert.eq(encryptedDocs.length,
                  1,
                  `Expected query ${tojson(query)} to only return one document. Found ${
                      encryptedDocs.length}`);
        let unEncryptedDocs = this._edb.getCollection(coll).find(query).toArray();
        assert.eq(unEncryptedDocs.length, 1);

        let encryptedDoc = encryptedDocs[0];
        let unEncryptedDoc = unEncryptedDocs[0];

        assert(encryptedDoc[kSafeContentField] !== undefined);

        for (let field in fields) {
            assert(encryptedDoc.hasOwnProperty(field),
                   `Could not find ${field} in encrypted ${tojson(encryptedDoc)}`);
            assert(unEncryptedDoc.hasOwnProperty(field),
                   `Could not find ${field} in unEncrypted ${tojson(unEncryptedDoc)}`);

            let rawField = encryptedDoc[field];
            assertIsIndexedEncryptedField(rawField);

            let unEncryptedField = unEncryptedDoc[field];
            assert.eq(unEncryptedField, fields[field]);
        }
    }

    assertWriteCommandReplyFields(response) {
        if (isMongod(this._edb)) {
            // These fields are replica set specific
            assert(response.hasOwnProperty("electionId"));
            assert(response.hasOwnProperty("opTime"));
        }

        assert(response.hasOwnProperty("$clusterTime"));
        assert(response.hasOwnProperty("operationTime"));
    }

    /**
     * Take a snapshot of a collection sorted by _id, run a operation, take a second snapshot.
     *
     * Ensure that the documents listed by index in unchangedDocumentIndexArray remain unchanged.
     * Ensure that the documents listed by index in changedDocumentIndexArray are changed.
     *
     * @param {string} collName
     * @param {Array} unchangedDocumentIndexArray
     * @param {Array} changedDocumentIndexArray
     * @param {Function} func
     * @returns
     */
    assertDocumentChanges(collName, unchangedDocumentIndexArray, changedDocumentIndexArray, func) {
        let coll = this._edb.getCollection(collName);

        let beforeDocuments = coll.find({}).sort({_id: 1}).toArray();

        let x = func();

        let afterDocuments = coll.find({}).sort({_id: 1}).toArray();

        for (let unchangedDocumentIndex of unchangedDocumentIndexArray) {
            assert.eq(beforeDocuments[unchangedDocumentIndex],
                      afterDocuments[unchangedDocumentIndex],
                      "Expected document index '" + unchangedDocumentIndex + "' to be the same." +
                          tojson(beforeDocuments[unchangedDocumentIndex]) + "\n==========\n" +
                          tojson(afterDocuments[unchangedDocumentIndex]));
        }

        for (let changedDocumentIndex of changedDocumentIndexArray) {
            assert.neq(
                beforeDocuments[changedDocumentIndex],
                afterDocuments[changedDocumentIndex],
                "Expected document index '" + changedDocumentIndex +
                    "' to be different. == " + tojson(beforeDocuments[changedDocumentIndex]) +
                    "\n==========\n" + tojson(afterDocuments[changedDocumentIndex]));
        }

        return x;
    }

    /**
     * Verify that the collection 'collName' contains exactly the documents 'docs'.
     *
     * @param {string} collName
     * @param {Array} docs
     * @returns
     */
    assertEncryptedCollectionDocuments(collName, docs) {
        let coll = this._edb.getCollection(collName);

        let onDiskDocs = coll.find({}, {[kSafeContentField]: 0}).sort({_id: 1}).toArray();

        assert.docEq(onDiskDocs, docs);
    }

    assertStateCollectionsAfterCompact(collName, ecocExists) {
        const baseCollInfos = this._edb.getCollectionInfos({"name": collName});
        assert.eq(baseCollInfos.length, 1);
        const baseCollInfo = baseCollInfos[0];
        assert(baseCollInfo.options.encryptedFields !== undefined);

        const checkMap = {};

        // Always expect ESC and ECC collections, optionally expect ECOC.
        // ECOC is not expected in sharded clusters.
        checkMap[baseCollInfo.options.encryptedFields.escCollection] = true;
        checkMap[baseCollInfo.options.encryptedFields.eccCollection] = true;
        checkMap[baseCollInfo.options.encryptedFields.ecocCollection] = ecocExists;
        checkMap[baseCollInfo.options.encryptedFields.ecocCollection + ".compact"] = false;

        const edb = this._edb;
        Object.keys(checkMap).forEach(function(coll) {
            const info = edb.getCollectionInfos({"name": coll});
            const msg = coll + (checkMap[coll] ? " does not exist" : " exists") + " after compact";
            assert.eq(info.length, checkMap[coll], msg);
        });
    }
};

function runEncryptedTest(db, dbName, collName, encryptedFields, runTestsCallback) {
    const dbTest = db.getSiblingDB(dbName);
    dbTest.dropDatabase();

    // Delete existing keyIds from encryptedFields to force
    // EncryptedClient to generate new keys on the new DB.
    for (let field of encryptedFields.fields) {
        if (field.hasOwnProperty("keyId")) {
            delete field.keyId;
        }
    }

    let client = new EncryptedClient(db.getMongo(), dbName);

    assert.commandWorked(
        client.createEncryptionCollection(collName, {encryptedFields: encryptedFields}));

    let edb = client.getDB();
    runTestsCallback(edb, client);
}

/**
 * @returns Returns true if talking to a sharded cluster
 */
function isFLE2ShardingEnabled() {
    return typeof (testingFLESharding) == "undefined" || testingFLESharding === true;
}

/**
 * @returns Returns true if talking to a replica set
 */
function isFLE2ReplicationEnabled() {
    return typeof (testingReplication) == "undefined" || testingReplication === true;
}

// TODO SERVER-67760 remove once feature flag is gone

/**
 * @returns Returns true if featureFlagFLE2Range is enabled
 */
function isFLE2RangeEnabled() {
    return typeof (testingFLE2Range) !== "undefined" && testingFLE2Range &&
        (TestData == undefined || TestData.setParameters.featureFlagFLE2Range);
}
/**
 * Assert a field is an indexed encrypted field. That includes both
 * equality and range
 *
 * @param {BinData} value bindata value
 */
function assertIsIndexedEncryptedField(value) {
    assert(value instanceof BinData, "Expected BinData, found: " + value);
    assert.eq(value.subtype(), 6, "Expected Encrypted bindata: " + value);
    assert(value.hex().startsWith("07") || value.hex().startsWith("09"),
           "Expected subtype 7 or 9 but found the wrong type: " + value.hex());
}

/**
 * Assert a field is an equality indexed encrypted field
 *
 * @param {BinData} value bindata value
 */
function assertIsEqualityIndexedEncryptedField(value) {
    assert(value instanceof BinData, "Expected BinData, found: " + value);
    assert.eq(value.subtype(), 6, "Expected Encrypted bindata: " + value);
    assert(value.hex().startsWith("07"),
           "Expected subtype 7 but found the wrong type: " + value.hex());
}

/**
 * Assert a field is a range indexed encrypted field
 *
 * @param {BinData} value bindata value
 */
function assertIsRangeIndexedEncryptedField(value) {
    assert(value instanceof BinData, "Expected BinData, found: " + value);
    assert.eq(value.subtype(), 6, "Expected Encrypted bindata: " + value);
    assert(value.hex().startsWith("09"),
           "Expected subtype 9 but found the wrong type: " + value.hex());
}

/**
 * Assert a field is an unindexed encrypted field
 *
 * @param {BinData} value bindata value
 */
function assertIsUnindexedEncryptedField(value) {
    assert(value instanceof BinData, "Expected BinData, found: " + value);
    assert.eq(value.subtype(), 6, "Expected Encrypted bindata: " + value);
    assert(value.hex().startsWith("06"),
           "Expected subtype 6 but found the wrong type: " + value.hex());
}
