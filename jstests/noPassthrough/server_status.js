// Checks storage-engine specific sections of db.severStatus() output.

(function() {
    'use string';

    var baseName = "jstests_server_status";
    var port = allocatePorts(1)[0] ;
    var dbpath = MongoRunner.dataPath + baseName + '/';

    // 'backgroundFlushing' is mmapv1-specific.
    var mongo = startMongodEmpty('--port', port, '--dbpath', dbpath, '--smallfiles');
    var testDB = mongo.getDB('test');
    var serverStatus = assert.commandWorked(testDB.serverStatus());
    if (serverStatus.storageEngine.name == 'mmapv1') {
        assert(serverStatus.backgroundFlushing,
               'mmapv1 db.serverStatus() result must contain backgroundFlushing document: ' +
               tojson(serverStatus));
    }
    else {
        assert(!serverStatus.backgroundFlushing,
               'Unexpected backgroundFlushing document in non-mmapv1 db.serverStatus() result: ' +
               tojson(serverStatus));
    }
    stopMongod(port);

    // 'dur' is mmapv1-specific and should only be generated when journaling is enabled.
    mongo = startMongodEmpty('--port', port, '--dbpath', dbpath, '--smallfiles', '--journal');
    testDB = mongo.getDB('test');
    serverStatus = assert.commandWorked(testDB.serverStatus());
    if (serverStatus.storageEngine.name == 'mmapv1') {
        assert(serverStatus.dur,
               'mmapv1 db.serverStatus() result must contain "dur" document: ' +
               tojson(serverStatus));
    }
    else {
        assert(!serverStatus.dur,
               'Unexpected "dur" document in non-mmapv1 db.serverStatus() result: ' +
               tojson(serverStatus));
    }
    stopMongod(port);
    mongo = startMongodEmpty('--port', port, '--dbpath', dbpath, '--smallfiles', '--nojournal');
    testDB = mongo.getDB('test');
    serverStatus = assert.commandWorked(testDB.serverStatus());
    assert(!serverStatus.dur,
           'Unexpected "dur" document in db.serverStatus() result when journaling is disabled: ' +
           tojson(serverStatus));
    stopMongod(port);
}());
