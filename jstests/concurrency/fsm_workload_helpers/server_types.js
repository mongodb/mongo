/**
 * Returns true if the process is a mongos, and false otherwise.
 *
 */
export function isMongos(db) {
    // Run isMaster directly on the database's session's client to bypass any session machinery.
    const res = assert.commandWorked(db.getSession().getClient().adminCommand('ismaster'));
    return 'isdbgrid' === res.msg;
}

/**
 * Returns true if the process is a mongod, and false otherwise.
 *
 */
export function isMongod(db) {
    return !isMongos(db);
}

/**
 * Returns true if the process is a mongod configsvr, and false otherwise.
 *
 */
export function isMongodConfigsvr(db) {
    if (!isMongod(db)) {
        return false;
    }
    var res = db.adminCommand('getCmdLineOpts');
    assert.commandWorked(res);

    return res.parsed && res.parsed.sharding && res.parsed.sharding.clusterRole === 'configsvr';
}

/**
 * Returns the name of the current storage engine.
 *
 * Throws an error if db is connected to a mongos, or if there is no reported storage engine.
 */
export function getStorageEngineName(db) {
    var status = db.serverStatus();
    assert.commandWorked(status);

    assert(isMongod(db), 'no storage engine is reported when connected to mongos');
    assert.neq(
        'undefined', typeof status.storageEngine, 'missing storage engine info in server status');

    return status.storageEngine.name;
}

/**
 * Returns true if the current storage engine is wiredTiger, and false otherwise.
 */
export function isWiredTiger(db) {
    return getStorageEngineName(db) === 'wiredTiger';
}

/**
 * Returns true if the current storage engine is ephemeral, and false otherwise.
 */
export function isEphemeral(db) {
    var engine = getStorageEngineName(db);
    return engine === 'inMemory';
}

/**
 * Returns true if the current storage engine supports committed reads.
 *
 * Throws an error if db is connected to a mongos, or if there is no reported storage engine.
 */
export function supportsCommittedReads(db) {
    var status = db.serverStatus();
    assert.commandWorked(status);

    assert(isMongod(db), 'no storage engine is reported when connected to mongos');
    assert.neq(
        'undefined', typeof status.storageEngine, 'missing storage engine info in server status');

    return status.storageEngine.supportsCommittedReads;
}
