// Term counter should be present in oplog entries under protocol version 1 but should be absent
// protocol version 0.
(function() {
    'use strict';
    load('jstests/replsets/rslib.js');

    var name = 'oplog_term';
    var replSet = new ReplSetTest({name: name, nodes: 1, protocolVersion: 0});
    replSet.startSet();
    replSet.initiate();
    replSet.waitForState(replSet.nodes[0], ReplSetTest.State.PRIMARY, 5 * 1000);

    // Protocol version 0 - 'term' field should be absent from oplog entry.
    var primary = replSet.getPrimary();
    var collection = primary.getDB('test').getCollection(name);
    assert.writeOK(collection.save({_id: 1}));

    var oplogEntry = getLatestOp(primary);
    assert(oplogEntry, 'unexpected empty oplog');
    assert.eq(collection.getFullName(),
              oplogEntry.ns,
              'unexpected namespace in oplog entry: ' + tojson(oplogEntry));
    assert.eq(
        1,
        oplogEntry.o._id,
        'oplog entry does not refer to most recently inserted document: ' + tojson(oplogEntry));
    assert(!oplogEntry.hasOwnProperty('t'),
           'oplog entry must not contain term: ' + tojson(oplogEntry));

    // Protocol version 1 - 'term' field should present in oplog entry.
    var config = assert.commandWorked(primary.adminCommand({replSetGetConfig: 1})).config;
    config.protocolVersion = 1;
    config.version++;
    assert.commandWorked(primary.adminCommand({replSetReconfig: config}));
    replSet.waitForState(replSet.nodes[0], ReplSetTest.State.PRIMARY, 5 * 1000);

    primary = replSet.getPrimary();
    collection = primary.getDB('test').getCollection(name);
    assert.writeOK(collection.save({_id: 2}));

    oplogEntry = getLatestOp(primary);
    assert(oplogEntry, 'unexpected empty oplog');
    assert.eq(collection.getFullName(),
              oplogEntry.ns,
              'unexpected namespace in oplog entry: ' + tojson(oplogEntry));
    assert.eq(
        2,
        oplogEntry.o._id,
        'oplog entry does not refer to most recently inserted document: ' + tojson(oplogEntry));
    assert(oplogEntry.hasOwnProperty('t'), 'oplog entry must contain term: ' + tojson(oplogEntry));

    var status = assert.commandWorked(primary.adminCommand({replSetGetStatus: 1}));
    assert.eq(status.term,
              oplogEntry.t,
              'term in oplog entry does not match term in status: ' + tojson(oplogEntry));
})();
