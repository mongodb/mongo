'use strict';

/**
 * Returns true if the process is a mongod, and false otherwise.
 *
 * 'dbOrServerStatus' can either be a server connection,
 * or the result of the { serverStatus: 1 } command.
 */
function isMongod(dbOrServerStatus) {
    var status = dbOrServerStatus;

    if (dbOrServerStatus instanceof DB) {
        var db = dbOrServerStatus;
        status = db.serverStatus();
    }

    return status.process === 'mongod';
}

/**
 * Returns true if the process is a mongos, and false otherwise.
 *
 * 'dbOrServerStatus' can either be a server connection,
 * or the result of the { serverStatus: 1 } command.
 */
function isMongos(dbOrServerStatus) {
    var status = dbOrServerStatus;

    if (dbOrServerStatus instanceof DB) {
        var db = dbOrServerStatus;
        status = db.serverStatus();
    }

    return status.process === 'mongos';
}

/**
 * Returns true if the current storage engine is mmapv1,
 * and false otherwise.
 *
 * 'dbOrServerStatus' must refer to a mongod connection
 * (and not a mongos connection), or the result of the
 * { serverStatus: 1 } command.
 */
function isMMAPv1(dbOrServerStatus) {
    var status = dbOrServerStatus;

    if (dbOrServerStatus instanceof DB) {
        var db = dbOrServerStatus;
        status = db.serverStatus();
    }

    // No storage engine is reported when connected to a mongos
    assert(isMongod(status), 'expected status of mongod process');
    assert.neq('undefined', typeof status.storageEngine,
               'missing storage engine info in server status');

    return status.storageEngine.name === 'mmapv1';
}

/**
 * Returns true if the current storage engine is wiredTiger
 * and false otherwise.
 *
 * 'dbOrServerStatus' must refer to a mongod connection
 * (and not a mongos connection), or the result of the
 * { serverStatus: 1 } command.
 */
function isWiredTiger(dbOrServerStatus) {
    var status = dbOrServerStatus;

    if (dbOrServerStatus instanceof DB) {
        var db = dbOrServerStatus;
        status = db.serverStatus();
    }

    // No storage engine is reported when connected to a mongos
    assert(isMongod(status), 'expected status of mongod process');
    assert.neq('undefined', typeof status.storageEngine,
               'missing storage engine info in server status');

    return Array.contains(['wiredTiger'], status.storageEngine.name);
}
