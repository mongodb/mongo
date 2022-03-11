/**
 * Create a FLE client that has an unencrypted and encrypted client to the same database
 */
class EncryptedClient {
    /**
     * Create a new encrypted FLE connection to the target server with a local KMS
     *
     * @param {Mongo} conn Connection to mongod or mongos
     * @param {string} dbName Name of database to setup key vault in
     */
    constructor(conn, dbName) {
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

        var currentPort = conn.host.split(":")[1];
        var host = "localhost:" + currentPort;
        var shell = Mongo(host, clientSideFLEOptions);
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

        return this._edb.createEncryptedCollection(name, options);
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
        const cis = this._edb.getCollectionInfos({"name": name});
        assert.eq(cis.length, 1, `Expected to find one collection named '${name}'`);

        const ci = cis[0];
        assert(ci.hasOwnProperty("options"), `Expected collection '${name}' to have 'options'`);
        const options = ci.options;
        assert(options.hasOwnProperty("encryptedFields"),
               `Expected collection '${name}' to have 'encryptedFields'`);

        const ef = options.encryptedFields;

        const actualEdc = this._edb.getCollection(name).count();
        assert.eq(actualEdc,
                  expectedEdc,
                  `EDC document count is wrong: Actual ${actualEdc} vs Expected ${expectedEdc}`);

        const actualEsc = this._edb.getCollection(ef.escCollection).count();
        assert.eq(actualEsc,
                  expectedEsc,
                  `ESC document count is wrong: Actual ${actualEsc} vs Expected ${expectedEsc}`);

        const actualEcc = this._edb.getCollection(ef.eccCollection).count();
        assert.eq(actualEcc,
                  expectedEcc,
                  `ECC document count is wrong: Actual ${actualEcc} vs Expected ${expectedEcc}`);

        const actualEcoc = this._edb.getCollection(ef.ecocCollection).count();
        assert.eq(actualEcoc,
                  expectedEcoc,
                  `ECOC document count is wrong: Actual ${actualEcoc} vs Expected ${expectedEcoc}`);
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
        assert.eq(encryptedDocs.length, 1);
        let unEncryptedDocs = this._edb.getCollection(coll).find(query).toArray();
        assert.eq(unEncryptedDocs.length, 1);

        let encryptedDoc = encryptedDocs[0];
        let unEncryptedDoc = unEncryptedDocs[0];

        assert(encryptedDoc["__safeContent__"] !== undefined);

        for (let field in fields) {
            assert(encryptedDoc.hasOwnProperty(field),
                   `Could not find ${field} in raw ${tojson(encryptedDoc)}`);
            assert(unEncryptedDoc.hasOwnProperty(field),
                   `Could not find ${field} in unEncrypted ${tojson(unEncryptedDoc)}`);

            let rawField = encryptedDoc[field];
            assertIsIndexedEncryptedField(rawField);

            let unEncryptedField = unEncryptedDoc[field];
            assert.eq(unEncryptedField, fields[field]);
        }
    }
}

// TODO - remove this when the feature flag is removed
function isFLE2Enabled() {
    return TestData == undefined || TestData.setParameters.featureFlagFLE2;
}

/**
 * @returns Returns true if talking to a sharded cluster
 */
function isFLE2ShardingEnabled() {
    if (!isFLE2Enabled()) {
        return false;
    }

    return typeof (testingFLESharding) == "undefined" || testingFLESharding === true;
}

/**
 * @returns Returns true if talking to a replica set
 */
function isFLE2ReplicationEnabled() {
    if (!isFLE2Enabled()) {
        return false;
    }

    return typeof (testingReplication) == "undefined" || testingReplication === true;
}

/**
 * Assert a field is an indexed encrypted field
 *
 * @param {BinData} value bindata value
 */
function assertIsIndexedEncryptedField(value) {
    assert(value instanceof BinData, "Expected BinData, found: " + value);
    assert.eq(value.subtype(), 6, "Expected Encrypted bindata: " + value);
    assert(value.hex().startsWith("07"),
           "Expected subtype 7 but found the wrong type: " + value.hex());
}
