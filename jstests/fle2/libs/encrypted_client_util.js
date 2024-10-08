import {isMongod} from "jstests/concurrency/fsm_workload_helpers/server_types.js";

export function isEnterpriseShell() {
    return buildInfo().modules.includes("enterprise");
}

function assertEnterpriseShell() {
    if (!isEnterpriseShell()) {
        doassert("Test requires the enterprise module");
    }
}

/**
 * Run a lambda on an FLE/QE encryption aware connection
 * @param {*} edb database associated with connection that was setup by EncryptedClient
 * @param {*} func lambda to run under an encryption connection
 * @returns value from lambda
 */
export function runWithEncryption(edb, func) {
    try {
        assertEnterpriseShell();

        assert(!edb.getMongo().isAutoEncryptionEnabled(),
               "Cannot switch to encrypted connection on already encrypted connection. Do not " +
                   "nest calls to runWithEncryption.");

        edb.getMongo().toggleAutoEncryption(true);

        return func();
    } finally {
        edb.getMongo().toggleAutoEncryption(false);
    }
}

/**
 * A series of extensions to DBCollection and DB that toggle encryption on and off per operation.
 */
DBCollection.prototype.einsert = function(obj, options) {
    return runWithEncryption(this, () => {
        return this.insert(obj, options);
    });
};

DBCollection.prototype.einsertOne = function(document, options) {
    return runWithEncryption(this, () => {
        return this.insertOne(document, options);
    });
};

DBCollection.prototype.eupdateOne = function(filter, update, options) {
    return runWithEncryption(this, () => {
        return this.updateOne(filter, update, options);
    });
};

DBCollection.prototype.eupdate = function(query, updateSpec, upsert, multi) {
    return runWithEncryption(this, () => {
        return this.update(query, updateSpec, upsert, multi);
    });
};

DBCollection.prototype.edeleteOne = function(filter, options) {
    return runWithEncryption(this, () => {
        return this.deleteOne(filter, options);
    });
};

DBCollection.prototype.edeleteMany = function(filter, options) {
    return runWithEncryption(this, () => {
        return this.deleteMany(filter, options);
    });
};

DBCollection.prototype.ereplaceOne = function(filter, replacement, options) {
    return runWithEncryption(this, () => {
        return this.replaceOne(filter, replacement, options);
    });
};

DB.prototype.erunCommand = function(cmd, params) {
    return runWithEncryption(this, () => {
        return this.runCommand(cmd, params);
    });
};

DB.prototype.eadminCommand = function(cmd, params) {
    return runWithEncryption(this, () => {
        return this.adminCommand(cmd, params);
    });
};

DBCollection.prototype.ecount = function(filter) {
    return runWithEncryption(this, () => {
        return this.find(filter).toArray().length;
    });
};

// Note that efind does not exist since find executes
// lazily, not eagerly
DBCollection.prototype.efindOne = function(filter, projection, options, readConcern, collation) {
    return runWithEncryption(this, () => {
        return this.findOne(filter, projection, options, readConcern, collation);
    });
};

DBCollection.prototype.erunCommand = function(cmd, params) {
    return runWithEncryption(this, () => {
        return this.runCommand(cmd, params);
    });
};

/**
 * Create a FLE client that has an unencrypted and encrypted client to the same database
 */
export var kSafeContentField = "__safeContent__";

export var EncryptedClient = class {
    /**
     * Create a new encrypted FLE connection to the target server with a local KMS
     *
     * @param {Mongo} conn Connection to mongod or mongos
     * @param {string} dbName Name of database to setup key vault in
     * @param {string} userName user name used for authentication (optional).
     * @param {string} adminPwd Admin password used for authentication (optional).
     */
    constructor(conn, dbName, userName = undefined, adminPwd = undefined) {
        // Detect if jstests/libs/override_methods/implicitly_shard_accessed_collections.js is in
        // use
        this.useImplicitSharding =
            typeof globalThis.ImplicitlyShardAccessCollSettings !== "undefined";

        if (conn.isAutoEncryptionEnabled()) {
            this._keyVault = conn.getKeyVault();
            this._db = conn.getDB(dbName);
            this._admindb = conn.getDB("admin");
            return;
        }

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

        var shell = conn;

        // Detatch existing auto encryption options
        // This forces us to drop the schema cache which is important as some tests repeatedly
        // create collections with the same name
        if (conn.getAutoEncryptionOptions() !== undefined) {
            conn.unsetAutoEncryption();
        }

        assert(shell.setAutoEncryption(clientSideFLEOptions));

        shell.toggleAutoEncryption(true);
        var keyVault = shell.getKeyVault();
        shell.toggleAutoEncryption(false);

        this._admindb = conn.getDB("admin");
        this._db = shell.getDB(dbName);
        this._keyVault = keyVault;
    }

    /**
     * Run a lambda on an FLE/QE encryption aware connection
     * @param {*} func lambda to run under an encryption connection
     * @returns value from lambda
     */
    runEncryptionOperation(func) {
        return runWithEncryption(this._db, func);
    }

    /**
     * Return a database
     *
     * @returns Database
     */
    getDB() {
        return this._db;
    }

    /**
     * Creates a session on the encryptedClient.
     */
    startSession() {
        return this._db.getMongo().startSession();
    }

    /**
     * Return an encrypted database
     *
     * @returns Database
     */
    getAdminDB() {
        return this._admindb;
    }

    /**
     * @returns KeyVault
     */
    getKeyVault() {
        return this._keyVault;
    }

    /**
     * Get the namespaces of the state collections that are associated with the given
     * encrypted data collection namespace.
     * @param {string} name Name of the encrypted data collection
     * @returns Object with fields "esc" and "ecoc" whose values
     *          are the corresponding namespace strings.
     */
    getStateCollectionNamespaces(collName) {
        const baseCollInfos = this._db.getCollectionInfos({"name": collName});
        assert.eq(baseCollInfos.length, 1);
        const baseCollInfo = baseCollInfos[0];
        assert(baseCollInfo.options.encryptedFields !== undefined);
        return {
            esc: baseCollInfo.options.encryptedFields.escCollection,
            ecoc: baseCollInfo.options.encryptedFields.ecocCollection,
        };
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

        this.runEncryptionOperation(() => {
            for (let field of options.encryptedFields.fields) {
                if (!field.hasOwnProperty("keyId")) {
                    let testkeyId = this._keyVault.createKey("local", "ignored");
                    field["keyId"] = testkeyId;
                }
            }
        });

        assert.neq(options,
                   undefined,
                   `createEncryptedCollection expected an options object, it is undefined`);
        assert(
            options.hasOwnProperty("encryptedFields") && typeof options.encryptedFields == "object",
            `options must contain an encryptedFields document'`);

        const res = this.runEncryptionOperation(() => {
            return assert.commandWorked(this._db.createCollection(name, options));
        });
        let listCollCmdObj = {listCollections: 1, nameOnly: false, filter: {name: name}};
        const cis = assert.commandWorked(this._db.runCommand(listCollCmdObj));

        assert.eq(
            cis.cursor.firstBatch.length, 1, `Expected to find one collection named '${name}'`);

        const ci = cis.cursor.firstBatch[0];
        assert(ci.hasOwnProperty("options"), `Expected collection '${name}' to have 'options'`);
        const storedOptions = ci.options;
        assert(options.hasOwnProperty("encryptedFields"),
               `Expected collection '${name}' to have 'encryptedFields'`);
        const ef = storedOptions.encryptedFields;

        // All our tests use "last" as the key to query on so shard on "last" instead of "_id"
        if (this.useImplicitSharding) {
            let shardCollCmd = {
                shardCollection: this._db.getName() + "." + name,
                key: {last: "hashed"},
                collation: {locale: "simple"}
            };

            let resShard = this._db.adminCommand(shardCollCmd);

            jsTestLog("Sharding: " + tojson(shardCollCmd));
        }

        const indexOptions = [{"key": {__safeContent__: 1}, name: "__safeContent___1"}];
        const createIndexCmdObj = {createIndexes: name, indexes: indexOptions};
        assert.commandWorked(this._db.runCommand(createIndexCmdObj));
        let tenantOption = {clusteredIndex: {key: {_id: 1}, unique: true}};
        assert.commandWorked(this._db.createCollection(ef.escCollection, tenantOption));
        assert.commandWorked(this._db.createCollection(ef.ecocCollection, tenantOption));

        return res;
    }

    /**
     * Assert the number of documents in the EDC and state collections is correct.
     *
     * @param {object} collection Collection object for EDC
     * @param {number} edc Number of documents in EDC
     * @param {number} esc Number of documents in ESC
     * @param {number} ecoc Number of documents in ECOC
     */
    assertEncryptedCollectionCountsByObject(
        sessionDB, name, expectedEdc, expectedEsc, expectedEcoc) {
        let listCollCmdObj = {listCollections: 1, nameOnly: false, filter: {name: name}};

        const cis = assert.commandWorked(this._db.runCommand(listCollCmdObj));
        assert.eq(
            cis.cursor.firstBatch.length, 1, `Expected to find one collection named '${name}'`);

        const ci = cis.cursor.firstBatch[0];
        assert(ci.hasOwnProperty("options"), `Expected collection '${name}' to have 'options'`);
        const options = ci.options;
        assert(options.hasOwnProperty("encryptedFields"),
               `Expected collection '${name}' to have 'encryptedFields'`);

        function countDocuments(sessionDB, name) {
            // FLE2 tests are testing transactions and using the count command is not supported.
            // For the purpose of testing NTDI and unsigned security token we are going to simply
            // use the count command since we are not testing any transaction. Otherwise fall back
            // to use aggregation.
            return sessionDB.getCollection(name).countDocuments({});
        }

        const actualEdc = countDocuments(sessionDB, name);
        assert.eq(actualEdc,
                  expectedEdc,
                  `EDC document count is wrong: Actual ${actualEdc} vs Expected ${expectedEdc}`);

        const ef = options.encryptedFields;
        const actualEsc = countDocuments(sessionDB, ef.escCollection);
        assert.eq(actualEsc,
                  expectedEsc,
                  `ESC document count is wrong: Actual ${actualEsc} vs Expected ${expectedEsc}`);

        const actualEcoc = countDocuments(sessionDB, ef.ecocCollection);
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
     * @param {number} ecoc Number of documents in ECOC
     */
    assertEncryptedCollectionCounts(name, expectedEdc, expectedEsc, expectedEcoc) {
        this.assertEncryptedCollectionCountsByObject(
            this._db, name, expectedEdc, expectedEsc, expectedEcoc);
    }

    /**
     * Assert the number of non-anchor documents in the ESC associated with the given EDC
     * collection name matches the expected.
     *
     * @param {string} name Name of EDC
     * @param {number} expectedCount Number of non-anchors expected in ESC
     */
    assertESCNonAnchorCount(name, expectedCount) {
        const escName = this.getStateCollectionNamespaces(name).esc;
        const actualCount =
            this._db.getCollection(escName).countDocuments({"value": {"$exists": false}});
        assert.eq(
            actualCount,
            expectedCount,
            `ESC non-anchor count is wrong: Actual ${actualCount} vs Expected ${expectedCount}`);
    }

    /**
     * Get a single document from the collection with the specified query. Ensure it contains the
     specified fields when decrypted and that those fields are encrypted.

     * @param {string} coll
     * @param {object} query
     * @param {object} fields
     */
    assertOneEncryptedDocumentFields(coll, query, fields) {
        let cmd = {find: coll};
        if (query) {
            cmd.filter = query;
        }
        const encryptedDocs = assert.commandWorked(this._db.runCommand(cmd)).cursor.firstBatch;
        assert.eq(encryptedDocs.length,
                  1,
                  `Expected query ${tojson(query)} to only return one document. Found ${
                      encryptedDocs.length}`);
        const unEncryptedDocs = assert.commandWorked(this._db.erunCommand(cmd)).cursor.firstBatch;
        assert.eq(unEncryptedDocs.length, 1);

        const encryptedDoc = encryptedDocs[0];
        const unEncryptedDoc = unEncryptedDocs[0];

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
        if (isMongod(this._db) && !TestData.testingReplicaSetEndpoint) {
            // These fields are replica set specific. The replica set endpoint forces write commands
            // to go through the router which does not return these fields.
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
        let coll = this._db.getCollection(collName);

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
        this.runEncryptionOperation(() => {
            let coll = this._db.getCollection(collName);

            let onDiskDocs = coll.find({}, {[kSafeContentField]: 0}).sort({_id: 1}).toArray();

            assert.docEq(docs, onDiskDocs);
        });
    }

    assertStateCollectionsAfterCompact(collName, ecocExists, ecocTempExists = false) {
        const baseCollInfos = this._db.getCollectionInfos({"name": collName});
        assert.eq(baseCollInfos.length, 1);
        const baseCollInfo = baseCollInfos[0];
        assert(baseCollInfo.options.encryptedFields !== undefined);

        const checkMap = {};

        // Always expect the ESC collection, optionally expect ECOC.
        checkMap[baseCollInfo.options.encryptedFields.escCollection] = true;
        checkMap[baseCollInfo.options.encryptedFields.ecocCollection] = ecocExists;
        checkMap[baseCollInfo.options.encryptedFields.ecocCollection + ".compact"] = ecocTempExists;

        const db = this._db;
        Object.keys(checkMap).forEach(function(coll) {
            const info = db.getCollectionInfos({"name": coll});
            const msg = coll + (checkMap[coll] ? " does not exist" : " exists") + " after compact";
            assert.eq(info.length, checkMap[coll], msg);
        });
    }
};

export function runEncryptedTest(db, dbName, collNames, encryptedFields, runTestsCallback) {
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

    if (typeof collNames === "string") {
        collNames = [collNames];
    }

    for (let collName of collNames) {
        assert.commandWorked(
            client.createEncryptionCollection(collName, {encryptedFields: encryptedFields}));
    }

    let edb = client.getDB();
    client.runEncryptionOperation(() => {
        runTestsCallback(edb, client);
    });
}

/**
 * @returns Returns true if talking to a replica set
 */
export function isFLE2ReplicationEnabled() {
    return typeof (testingReplication) == "undefined" || testingReplication === true;
}

/**
 * @returns Returns true if internalQueryFLEAlwaysUseEncryptedCollScanMode is enabled
 */
export function isFLE2AlwaysUseCollScanModeEnabled(db) {
    const doc = assert.commandWorked(
        db.adminCommand({getParameter: 1, internalQueryFLEAlwaysUseEncryptedCollScanMode: 1}));
    return (doc.internalQueryFLEAlwaysUseEncryptedCollScanMode === true);
}

/**
 * Assert a field is an indexed encrypted field. That includes both
 * equality and range
 *
 * @param {BinData} value bindata value
 */
export function assertIsIndexedEncryptedField(value) {
    assert(value instanceof BinData, "Expected BinData, found: " + value);
    assert.eq(value.subtype(), 6, "Expected Encrypted bindata: " + value);
    assert(value.hex().startsWith("0e") || value.hex().startsWith("0f"),
           "Expected subtype 14 or 15 but found the wrong type: " + value.hex());
}

/**
 * Assert a field is an equality indexed encrypted field
 *
 * @param {BinData} value bindata value
 */
export function assertIsEqualityIndexedEncryptedField(value) {
    assert(value instanceof BinData, "Expected BinData, found: " + value);
    assert.eq(value.subtype(), 6, "Expected Encrypted bindata: " + value);
    assert(value.hex().startsWith("0e"),
           "Expected subtype 14 but found the wrong type: " + value.hex());
}

/**
 * Assert a field is a range indexed encrypted field
 *
 * @param {BinData} value bindata value
 */
export function assertIsRangeIndexedEncryptedField(value) {
    assert(value instanceof BinData, "Expected BinData, found: " + value);
    assert.eq(value.subtype(), 6, "Expected Encrypted bindata: " + value);
    assert(value.hex().startsWith("0f"),
           "Expected subtype 15 but found the wrong type: " + value.hex());
}

/**
 * Assert a field is an unindexed encrypted field
 *
 * @param {BinData} value bindata value
 */
export function assertIsUnindexedEncryptedField(value) {
    assert(value instanceof BinData, "Expected BinData, found: " + value);
    assert.eq(value.subtype(), 6, "Expected Encrypted bindata: " + value);
    assert(value.hex().startsWith("10"),
           "Expected subtype 16 but found the wrong type: " + value.hex());
}
