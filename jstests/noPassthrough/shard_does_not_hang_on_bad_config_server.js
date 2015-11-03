(function() {
'use strict';

var conn = MongoRunner.runMongod();
var connA = new Mongo(conn.host);
var connB = new Mongo(conn.host);

var res;

res = assert.commandFailed(
    connA.adminCommand({ moveChunk: 'DummyDB.DummyColl',
                         find: { e: 0 },
                         to: 'DummyShard',
                         configdb: 'localhost:1' }));
assert.eq(ErrorCodes.ManualInterventionRequired, res.code);

res = assert.commandFailed(
    connB.adminCommand({ moveChunk: 'DummyDB.DummyColl',
                         find: { e: 0 },
                         to: 'DummyShard',
                         configdb: 'localhost:1' }));
assert.eq(ErrorCodes.ManualInterventionRequired, res.code);

MongoRunner.stopMongod(conn);

})();
