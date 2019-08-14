// oplog should contain the field "wt" with wallClock timestamps.
(function() {
'use strict';
load('jstests/replsets/rslib.js');

var assertLastOplogHasWT = function(primary, msg) {
    const opLogEntry = getLatestOp(primary);
    assert(opLogEntry.hasOwnProperty('wall'),
           'oplog entry must contain wt field: ' + tojson(opLogEntry));
};

var name = 'wt_test_coll';
var replSet = new ReplSetTest({nodes: 1, oplogSize: 2});
replSet.startSet();
replSet.initiate();

var primary = replSet.getPrimary();
var collection = primary.getDB('test').getCollection(name);

assert.commandWorked(collection.insert({_id: 1, val: 'x'}));
assertLastOplogHasWT(primary, 'insert');

assert.commandWorked(collection.update({_id: 1}, {val: 'y'}));
assertLastOplogHasWT(primary, 'update');

assert.commandWorked(collection.remove({_id: 1}));
assertLastOplogHasWT(primary, 'remove');

replSet.stopSet();
})();
