// Test for cloning a db from a replica set [SERVER-1643] -Tony

load('jstests/libs/grid.js')

doTest = function( signal ) {

    var N = 2000

    print("~1KB string");
    var Text = ''
    for (var i = 0; i < 40; i++)
        Text += 'abcdefghijklmnopqrstuvwxyz'

    print("Create replica set");
    var repset = new ReplicaSet ('testSet', 3) .begin()
    var master = repset.getMaster()
    var db1 = master.getDB('test')
    
    print("Insert data");
    var bulk = db1['foo'].initializeUnorderedBulkOp();
    for (var i = 0; i < N; i++) {
        bulk.insert({ x: i, text: Text });
    }
    assert.writeOK(bulk.execute());
    
    print("Create single server");
    var solo = new Server ('singleTarget')
    var soloConn = solo.begin()
    soloConn.getDB("admin").runCommand({setParameter:1,logLevel:5});
    
    var db2 = soloConn.getDB('test')
    
    print("Clone db from replica set to single server");
    db2.cloneDatabase (repset.getURL())
    
    print("Confirm clone worked");
    assert.eq (Text, db2['foo'] .findOne({x: N-1}) ['text'], 'cloneDatabase failed (test1)')
    
    print("Now test the reverse direction");
    db1 = master.getDB('test2')
    db2 = soloConn.getDB('test2')

    bulk = db2['foo'].initializeUnorderedBulkOp();
    for (var i = 0; i < N; i++) {
        bulk.insert({ x: i, text: Text });
    }
    assert.writeOK(bulk.execute());
    db1.cloneDatabase (solo.host())
    assert.eq (Text, db2['foo'] .findOne({x: N-1}) ['text'], 'cloneDatabase failed (test2)')

    print("Shut down replica set and single server");
    solo.end()
    repset.stopSet( signal )
}

if (jsTest.options().keyFile || jsTest.options().useX509) {
    print("Skipping test because clone command doesn't work with authentication enabled: SERVER-4245")
} else {
    doTest( 15 );
    print("replsets/cloneDb.js SUCCESS");
}
