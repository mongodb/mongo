// Test for cloning a db from a replica set [SERVER-1643]

load('jstests/libs/grid.js')

doTest = function( signal ) {

    var N = 2000

    // ~1KB string
    var Text = ''
    for (var i = 0; i < 40; i++)
        Text += 'abcdefghijklmnopqrstuvwxyz'

    // Create replica set
    var repset = new ReplicaSet ('testSet', 3) .start()
    var master = repset.getMaster()
    var db1 = master.getDB('test')
    
    // Insert data
    for (var i = 0; i < N; i++) {
        db1['foo'].insert({x: i, text: Text})
        db1.getLastError(2)  // wait to be copied to at least one secondary
    }
    
    // Create single server
    var solo = new Server ('singleTarget')
    var soloConn = solo.start()
    var db2 = soloConn.getDB('test')
    
    // Clone db from replica set to single server
    db2.cloneDatabase (repset.getURL())
    
    // Confirm clone worked
    assert.eq (Text, db2['foo'] .findOne({x: N-1}) ['text'], 'cloneDatabase failed (test1)')
    
    // Now test the reverse direction
    db1 = master.getDB('test2')
    db2 = soloConn.getDB('test2')
    for (var i = 0; i < N; i++) {
        db2['foo'].insert({x: i, text: Text})
        db2.getLastError()
    }
    db1.cloneDatabase (solo.host())
    assert.eq (Text, db2['foo'] .findOne({x: N-1}) ['text'], 'cloneDatabase failed (test2)')

    // Shut down replica set and single server
    repset.stopSet( signal )
    stopMongod (solo.port, signal)
}

doTest( 15 );
print("replsets/cloneDb.js SUCCESS");
