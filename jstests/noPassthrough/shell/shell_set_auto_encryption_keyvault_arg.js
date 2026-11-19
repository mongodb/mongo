/**
 * Mongo.setAutoEncryption() must reject a key vault argument that is not a Mongo connection
 * instead of dereferencing a null connection pointer and crashing the shell.
 */
const conn = MongoRunner.runMongod();

for (const badKeyVault of [{}, [], Mongo.prototype, conn.getDB("test")]) {
    assert.throwsWithCode(() => conn.setAutoEncryption({}, badKeyVault), ErrorCodes.BadValue);
}

assert.throwsWithCode(() => conn.setAutoEncryption({}, 1), 7760002);

// A real Mongo connection is accepted. Passing no encryption options makes setAutoEncryption()
// return false without configuring anything.
const keyVaultConn = new Mongo(conn.host);
assert.eq(false, conn.setAutoEncryption(undefined, keyVaultConn));
assert.eq(false, conn.isAutoEncryptionEnabled());

MongoRunner.stopMongod(conn);
