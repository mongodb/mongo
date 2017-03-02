// Checks storage-engine specific sections of db.severStatus() output.

(function() {
    'use strict';

    // 'backgroundFlushing' is mmapv1-specific.
    var bongo = BongoRunner.runBongod({smallfiles: ""});
    var testDB = bongo.getDB('test');
    var serverStatus = assert.commandWorked(testDB.serverStatus());
    if (serverStatus.storageEngine.name == 'mmapv1') {
        assert(serverStatus.backgroundFlushing,
               'mmapv1 db.serverStatus() result must contain backgroundFlushing document: ' +
                   tojson(serverStatus));
    } else {
        assert(!serverStatus.backgroundFlushing,
               'Unexpected backgroundFlushing document in non-mmapv1 db.serverStatus() result: ' +
                   tojson(serverStatus));
    }
    BongoRunner.stopBongod(bongo);

    // 'dur' is mmapv1-specific and should only be generated when journaling is enabled.
    bongo = BongoRunner.runBongod({smallfiles: "", journal: ""});
    testDB = bongo.getDB('test');
    serverStatus = assert.commandWorked(testDB.serverStatus());
    if (serverStatus.storageEngine.name == 'mmapv1') {
        assert(
            serverStatus.dur,
            'mmapv1 db.serverStatus() result must contain "dur" document: ' + tojson(serverStatus));
    } else {
        assert(!serverStatus.dur,
               'Unexpected "dur" document in non-mmapv1 db.serverStatus() result: ' +
                   tojson(serverStatus));
    }
    BongoRunner.stopBongod(bongo);
    bongo = BongoRunner.runBongod({smallfiles: "", nojournal: ""});
    testDB = bongo.getDB('test');
    serverStatus = assert.commandWorked(testDB.serverStatus());
    assert(!serverStatus.dur,
           'Unexpected "dur" document in db.serverStatus() result when journaling is disabled: ' +
               tojson(serverStatus));
    BongoRunner.stopBongod(bongo);
}());
