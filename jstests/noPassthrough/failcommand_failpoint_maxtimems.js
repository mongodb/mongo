(function() {
'use strict';

var conn = MongoRunner.runMongod();
var admin = conn.getDB('admin');
var db = conn.getDB('test');

var fpCmd = {
    configureFailPoint: 'failCommand',
    mode: {times: 1},
    data: {
        failCommands: ['insert'],
        blockConnection: true,
        blockTimeMS: 1000,
    },
};

assert.commandWorked(admin.runCommand(fpCmd));

var insertCmd = {
    insert: 'coll',
    documents: [{x: 1}],
    maxTimeMS: 100,
};

assert.commandFailedWithCode(db.runCommand(insertCmd), ErrorCodes.MaxTimeMSExpired);

MongoRunner.stopMongod(conn);
})();
