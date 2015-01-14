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
}());
