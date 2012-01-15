// test basic currentop functionality + querying of nested documents
t = db.jstests_currentop
t.drop();

for(i=0;i<100;i++) {
    t.save({ "num": i });
}
// Make sure data is written before we start reading it in parallel shells.
db.getLastError();

function ops(q) {
    return db.currentOp(q).inprog;
}

// sleep for a second for each (of 100) documents; can be killed in between documents & test should complete before 100 seconds 
s1 = startParallelShell( "db.jstests_currentop.count( { '$where': function() { sleep(1000); } } )" );
// need to wait for read to start
assert.soon( function(){
        return ops( { "lockType": "read", "ns": "test.jstests_currentop" } ).length >= 1;
    } );
s2 = startParallelShell( "db.jstests_currentop.update( { '$where': function() { sleep(1000); } }, { 'num': 1 }, false, true )" );

o = [];
assert.soon( function() {
    o = ops({ "ns": "test.jstests_currentop" });

    var writes = ops({ "lockType": "write", "ns": "test.jstests_currentop" }).length;
    var reads = ops({ "lockType": "read", "ns": "test.jstests_currentop" }).length;

    print( "total: " + o.length + " w: " + writes + " r:" + reads );

    return o.length > writes && o.length > reads;
} );

// avoid waiting for the operations to complete (if soon succeeded)
for(var i in o) {
    db.killOp(o[i].opid);
}

start = new Date();

s1();
s2();

// don't want to pass if timeout killed the js function
assert( ( new Date() ) - start < 30000 );
