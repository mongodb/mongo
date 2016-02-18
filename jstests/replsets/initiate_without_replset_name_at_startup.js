/**
 * If a node is started with --configsvr but without --replSet, and it has neither an oplog nor a
 * local replica set  config document, the operator should be able to run replSetInitiate to create
 * a single-node replica set configuration and to prime the oplog. Then, on restart if the operator
 * adds --replSet to the startup configuration directives, the node can immediately transition into
 * PRIMARY rather than waiting in STARTUP for the operator to run replSetInitiate. This should
 * only be allowed for single-node replica set configurations.
 *
 * This test cannot be run on ephemeral storage engines, since their replica set config document
 * will not persist across a restart and they will not transition to PRIMARY as described above.
 * @tags: [requires_persistence]
 */
(function () {
    "use strict";

    var baseName = 'testInitiateWithoutReplSetNameAtStartup';
    var port = allocatePorts(1)[0];
    var dbpath = MongoRunner.dataPath + baseName + '/';

    var mongod = MongoRunner.runMongod({
        dbpath: dbpath,
        port: port});

    var config = {
        _id: baseName,
        version: 1,
        members: [
            {_id: 0, host: mongod.name},
        ],
    };

    var result = assert.commandFailedWithCode(
        mongod.getDB('admin').runCommand({replSetInitiate: config}),
        ErrorCodes.NoReplicationEnabled,
        'replSetInitiate should fail when both --configsvr and --replSet are missing.');
    assert(
        result.errmsg.match(/This node was not started with the replSet option/),
        'unexpected error message when both --configsvr and --replSet are missing. ' +
        'configuration: ' + tojson(result));

    // The rest of this test can only be run if the storageEngine supports committed reads.
    var supportsCommittedReads =
        mongod.getDB('admin').serverStatus().storageEngine.supportsCommittedReads;

    MongoRunner.stopMongod(port);

    if (!supportsCommittedReads) {
        jsTestLog('Skipping rest of test because storage engine does not support committed reads');
        return;
    }

    mongod = MongoRunner.runMongod({
        configsvr: '',
        dbpath: dbpath,
        port: port,
        restart: true});

    assert.commandWorked(
        mongod.getDB('admin').runCommand({replSetInitiate: config}),
        'replSetInitiate should not fail when given a valid configuration');

    // Check saved config
    var systemReplsetCollection = mongod.getDB('local').system.replset;
    assert.eq(1, systemReplsetCollection.count(),
              'replSetInitiate did not save configuration in ' +
              systemReplsetCollection.getFullName());
    var savedConfig = systemReplsetCollection.findOne();
    assert.eq(config._id, savedConfig._id,
              'config passed to replSetInitiate (left side) does not match config saved in ' +
              systemReplsetCollection.getFullName() + ' (right side)');

    result = assert.commandFailedWithCode(
        mongod.getDB('admin').runCommand({replSetInitiate: {
            _id: baseName + '-2',
            version: 1,
            members: [
                {_id: 0, host: mongod.name},
            ],
        }}),
        ErrorCodes.AlreadyInitialized,
        'expected AlreadyInitialized error code when configuration already exists in ' +
        systemReplsetCollection.getFullName());
    assert(result.errmsg.match(/already initialized/),
           'unexpected error message when replica set configuration already exists ' +
           tojson(result));
    systemReplsetCollection = mongod.getDB('local').system.replset;
    savedConfig = systemReplsetCollection.findOne();
    assert.eq(config._id, savedConfig._id,
              'config passed to replSetInitiate (left side) does not match config saved in ' +
              systemReplsetCollection.getFullName() + ' (right side)');

    var oplogCollection = mongod.getDB('local').oplog.rs;
    assert(oplogCollection.exists(),
           'oplog collection ' + oplogCollection.getFullName() +
           ' not created after successful replSetInitiate. Collections in local database: ' +
           mongod.getDB('local').getCollectionNames().join(', '));
    assert(oplogCollection.isCapped(),
           'oplog collection ' + oplogCollection.getFullName() + ' must be capped');
    assert.eq(1, oplogCollection.count(),
              'oplog collection ' + oplogCollection.getFullName() +
              ' is not initialized with first entry.');
    var oplogEntry = oplogCollection.findOne();
    assert.eq('n', oplogEntry.op, 'unexpected first oplog entry type: ' + tojson(oplogEntry));

    MongoRunner.stopMongod(port);

    // Restart server and attempt to save a different config.
    mongod = MongoRunner.runMongod({
        configsvr: '',
        dbpath: dbpath,
        port: port,
        restart: true});
    result = assert.commandFailedWithCode(
        mongod.getDB('admin').runCommand({replSetInitiate: {
            _id: baseName + '-2',
            version: 1,
            members: [
                {_id: 0, host: mongod.name},
            ],
        }}),
        ErrorCodes.AlreadyInitialized,
        'expected AlreadyInitialized error code when configuration already exists in ' +
        systemReplsetCollection.getFullName() + ' after restarting');
    assert(result.errmsg.match(/already initialized/),
           'unexpected error message when replica set configuration already exists ' +
           '(after restarting without --replSet): ' + tojson(result));
    systemReplsetCollection = mongod.getDB('local').system.replset;
    savedConfig = systemReplsetCollection.findOne();
    assert.eq(config._id, savedConfig._id,
              'config passed to replSetInitiate (left side) does not match config saved in ' +
              systemReplsetCollection.getFullName() + ' (right side)');

    MongoRunner.stopMongod(port);

    // Restart server with --replSet and check own replica member state.
    mongod = MongoRunner.runMongod({
        configsvr: '',
        dbpath: dbpath,
        port: port,
        replSet: config._id,
        restart: true});

    // Wait for member state to become PRIMARY.
    assert.soon(
        function() {
            result = assert.commandWorked(
                mongod.getDB('admin').runCommand({replSetGetStatus: 1}),
                'failed to get replica set status after restarting server with --replSet option');
            assert.eq(1, result.members.length,
                      'replica set status should contain exactly 1 member');
            var member = result.members[0];
            print('Current replica member state = ' + member.state + ' (' + member.stateStr + ')');
            return member.state == ReplSetTest.State.PRIMARY;
        },
        'Replica set member state did not reach PRIMARY after starting up with --replSet option',
        5000,
        1000);

    // Write/read a single document to ensure basic functionality.
    var t = mongod.getDB('config').getCollection(baseName);
    var doc = {_id: 0};
    assert.soon(
        function() {
            result = t.save(doc);
            assert(result instanceof WriteResult);
            if (result.hasWriteError()) {
                print('Failed with write error saving document after transitioning to primary: ' +
                      tojson(result) + '. Retrying...');
                return false;
            }
            if (result.hasWriteConcernError()) {
                print('Failed with write concern error saving document after transitioning to ' +
                      'primary: ' + tojson(result) + '. Retrying...');
                return false;
            }
            print('Successfully saved document after transitioning to primary: ' + tojson(result));
            return true;
        },
        'failed to save document after transitioning to primary',
        5000,
        1000);

    assert.eq(1, t.count(), 'incorrect collection size after successful write');
    assert.eq(doc, t.findOne());

    oplogCollection = mongod.getDB('local').oplog.rs;
    oplogEntry = oplogCollection.find().sort({$natural: -1}).limit(1).next();
    assert.eq('i', oplogEntry.op, 'unexpected optype for insert oplog entry');
    assert.eq(t.getFullName(), oplogEntry.ns, 'unexpected namespace for insert oplog entry');
    assert.eq(doc, oplogEntry.o, 'unexpected embedded object for insert oplog entry');

    MongoRunner.stopMongod(port);
}());
