/** Test TTL docs are not deleted from secondaries directly
 */

var rt = new ReplSetTest( { name : "ttl_repl" , nodes: 2 } );

// setup set
var nodes = rt.startSet();
rt.initiate();
var master = rt.getMaster();
rt.awaitSecondaryNodes();
var slave1 = rt.getSecondary();

// shortcuts
var masterdb = master.getDB( 'd' );
var slave1db = slave1.getDB( 'd' );
var mastercol = masterdb[ 'c' ];
var slave1col = slave1db[ 'c' ];

// create TTL index, wait for TTL monitor to kick in, then check things
mastercol.ensureIndex( { x : 1 } , { expireAfterSeconds : 10 } );

rt.awaitReplication();

//increase logging
slave1col.getDB().adminCommand({setParameter:1, logLevel:1});

//insert old doc (10 minutes old) directly on secondary using godinsert
slave1col.runCommand("godinsert",
        {obj: {_id: new Date(), x: new Date( (new Date()).getTime() - 600000 ) } })
assert.eq(1, slave1col.count(), "missing inserted doc" );

sleep(70*1000) //wait for 70seconds
assert.eq(1, slave1col.count(), "ttl deleted my doc!" );

// looking for this error : "Assertion: 13312:replSet error : logOp() but not primary"
// indicating that the secondary tried to delete the doc, but shouldn't be writing
var errorString = "13312";
var foundError = false;
var globalLogLines = slave1col.getDB().adminCommand({getLog:"global"}).log
for (i in globalLogLines) {
    var line = globalLogLines[i];
    if (line.match( errorString )) {
        foundError = true;
        errorString = line; // replace error string with what we found.
        break;
    }
}

assert.eq(false, foundError, "found error in this line: " + errorString);

// finish up
rt.stopSet();