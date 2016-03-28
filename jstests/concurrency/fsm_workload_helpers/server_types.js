'use strict';

/**
 * Returns true if the process is a mongos, and false otherwise.
 *
 */
function isMongos(db) {
    var res = db.runCommand('ismaster');
    assert.commandWorked(res);

    return 'isdbgrid' === res.msg;
}

/**
 * Returns true if the process is a mongod, and false otherwise.
 *
 */
function isMongod(db) {
    return !isMongos(db);
}

/**
 * Returns the name of the current storage engine.
 *
 * Throws an error if db is connected to a mongos, or if there is no reported storage engine.
 */
function getStorageEngineName(db) {
    var status = db.serverStatus();
    assert.commandWorked(status);

    assert(isMongod(db), 'no storage engine is reported when connected to mongos');
    assert.neq(
        'undefined', typeof status.storageEngine, 'missing storage engine info in server status');

    return status.storageEngine.name;
}

/**
 * Returns true if the current storage engine is mmapv1, and false otherwise.
 */
function isMMAPv1(db) {
    return getStorageEngineName(db) === 'mmapv1';
}

/**
 * Returns true if an update can cause the RecordId of a document to change.
 */
function recordIdCanChangeOnUpdate(db) {
    // A RecordId on MMAPv1 is just its location on disk, which can change if the document grows and
    // needs to be moved.
    return isMMAPv1(db);
}

/**
 * Returns true if the current storage engine is wiredTiger, and false otherwise.
 */
function isWiredTiger(db) {
    return getStorageEngineName(db) === 'wiredTiger';
}

/**
 * Returns true if the current storage engine is ephemeral, and false otherwise.
 */
function isEphemeral(db) {
    var engine = getStorageEngineName(db);
    return (engine === 'inMemory') || (engine === 'ephemeralForTest');
}

/**
 * Returns true if the current storage engine supports document-level concurrency, and false
 * otherwise.
 */
function supportsDocumentLevelConcurrency(db) {
    var engine = getStorageEngineName(db);
    return ['wiredTiger', 'rocksdb', 'inMemory'].indexOf(engine) !== -1;
}
