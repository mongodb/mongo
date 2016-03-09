// TODO: remove test after we deprecate SyncClusterConnection

// Test for SERVER-11492 - make sure that upserting a new document reports n:1 in GLE

var test = new SyncCCTest("sync1");

if (test.conn.writeMode() == 'commands') {
    jsTest.log('Skipping test not compatible with write commands');
} else {
    var db = test.conn.getDB("test");
    var t = db.sync8;
    t.remove({});

    t.update({_id: 1}, {$set: {a: 1}}, true);
    var le = db.getLastErrorObj();
    assert.eq(1, le.n);

    test.stop();
}
