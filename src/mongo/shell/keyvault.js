//  Class that allows the mongo shell to talk to the mongodb KeyVault.
//  Loaded only into the enterprise module.

Mongo.prototype.getKeyVault = function () {
    return new KeyVault(this);
};

class KeyVault {
    _runCommand(client, func, args) {
        let numRetries = 3;
        do {
            try {
                const result = func.apply(client, args);
                return result;
            } catch (e) {
                numRetries--;
                if (!isNetworkError(e) || numRetries == 0) {
                    jsTest.log("KeyVault: We have exceeded the number of retries. Throwing.");
                    throw e;
                }

                const res = this.mongo.getDB("admin")._helloOrLegacyHello();
                if (!res) {
                    jsTest.log("KeyVault: We do not have a connection to the database. Throwing.");
                    throw e;
                }
                this.keyColl = this.mongo.getDataKeyCollection();
            }
        } while (true);
    }

    constructor(mongo) {
        this.mongo = mongo;
        let collection = this._runCommand(this.mongo, this.mongo.getDataKeyCollection, {});
        this.keyColl = collection;
        this._runCommand(this.keyColl, this.keyColl.createIndex, [
            {keyAltNames: 1},
            {unique: true, partialFilterExpression: {keyAltNames: {$exists: true}}},
        ]);
    }

    createKey(kmsProvider, param2 = undefined, param3 = undefined) {
        if (Array.isArray(param2) && param3 === undefined) {
            if (kmsProvider !== "local") {
                return "ValueError: customerMasterKey must be defined if kmsProvider is not local.";
            }
            return this._createKey(kmsProvider, "", param2);
        }

        return this._createKey(kmsProvider, param2, param3);
    }

    _createKey(kmsProvider, customerMasterKey, keyAltNames) {
        if (typeof kmsProvider !== "string") {
            return "TypeError: kmsProvider must be of String type.";
        }

        if (typeof customerMasterKey !== "string" && typeof customerMasterKey !== "object") {
            return "TypeError: customer master key must be of String type.";
        }

        let masterKeyAndMaterial = this._runCommand(this.mongo, this.mongo.generateDataKey, [
            kmsProvider,
            customerMasterKey,
        ]);
        let masterKey = masterKeyAndMaterial.masterKey;

        let current = ISODate();
        let uuid = UUID();

        let doc = {
            "_id": uuid,
            "keyMaterial": masterKeyAndMaterial.keyMaterial,
            "creationDate": current,
            "updateDate": current,
            "status": NumberInt(0),
            "version": NumberLong(0),
            masterKey,
        };

        if (keyAltNames) {
            if (!Array.isArray(keyAltNames)) {
                return "TypeError: key alternate names must be of Array type.";
            }

            let i = 0;
            for (i = 0; i < keyAltNames.length; i++) {
                if (typeof keyAltNames[i] !== "string") {
                    return "TypeError: items in key alternate names must be of String type.";
                }
            }

            doc.keyAltNames = keyAltNames;
        }

        let insertCmdObj = {
            insert: this.keyColl.getName(),
            documents: [doc],
            writeConcern: {w: "majority"},
        };

        assert.commandWorked(this._runCommand(this.keyColl.getDB(), this.keyColl.getDB().runCommand, [insertCmdObj]));
        return uuid;
    }

    getKey(keyId) {
        return this._runCommand(this.keyColl, this.keyColl.find, [{"_id": keyId}]);
    }

    getKeyByAltName(keyAltName) {
        return this._runCommand(this.keyColl, this.keyColl.find, [{"keyAltNames": keyAltName}]);
    }

    deleteKey(keyId) {
        return this._runCommand(this.keyColl, this.keyColl.deleteOne, [{"_id": keyId}]);
    }

    getKeys() {
        return this._runCommand(this.keyColl, this.keyColl.find, []);
    }

    addKeyAlternateName(keyId, keyAltName) {
        // keyAltName is not allowed to be an array or an object. In javascript,
        // typeof array is object.
        if (typeof keyAltName === "object") {
            return "TypeError: key alternate name cannot be object or array type.";
        }
        return this._runCommand(this.keyColl, this.keyColl.findAndModify, [
            {
                query: {"_id": keyId},
                update: {$push: {"keyAltNames": keyAltName}, $currentDate: {"updateDate": true}},
            },
        ]);
    }

    removeKeyAlternateName(keyId, keyAltName) {
        if (typeof keyAltName === "object") {
            return "TypeError: key alternate name cannot be object or array type.";
        }

        const ret = this._runCommand(this.keyColl, this.keyColl.findAndModify, [
            {
                query: {"_id": keyId},
                update: {$pull: {"keyAltNames": keyAltName}, $currentDate: {"updateDate": true}},
            },
        ]);

        if (ret != null && ret.keyAltNames.length === 1 && ret.keyAltNames[0] === keyAltName) {
            // Remove the empty array to prevent duplicate key violations
            return this._runCommand(this.keyColl, this.keyColl.findAndModify, [
                {
                    query: {"_id": keyId, "keyAltNames": undefined},
                    update: {$unset: {"keyAltNames": ""}, $currentDate: {"updateDate": true}},
                },
            ]);
        }
        return ret;
    }
}

class ClientEncryption {
    constructor(mongo) {
        this.mongo = mongo;
    }

    encrypt(...args) {
        return this.mongo.encrypt(...args);
    }

    decrypt(...args) {
        return this.mongo.decrypt(...args);
    }
}

Mongo.prototype.getClientEncryption = function () {
    return new ClientEncryption(this);
};
