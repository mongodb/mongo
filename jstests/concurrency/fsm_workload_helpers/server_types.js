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
 * Returns true if the current storage engine is mmapv1,
 * and false otherwise.
 *
 */
function isMMAPv1(db) {
    var status = db.serverStatus();
    assert.commandWorked(status);

    assert(isMongod(db),
           'no storage engine is reported when connected to mongos');
    assert.neq('undefined', typeof status.storageEngine,
               'missing storage engine info in server status');

    return status.storageEngine.name === 'mmapv1';
}

/**
 * Returns true if the current storage engine is wiredTiger
 * and false otherwise.
 *
 */
function isWiredTiger(db) {
    var status = db.serverStatus();
    assert.commandWorked(status);

    assert(isMongod(db),
           'no storage engine is reported when connected to mongos');
    assert.neq('undefined', typeof status.storageEngine,
               'missing storage engine info in server status');

    return status.storageEngine.name === 'wiredTiger';
}
