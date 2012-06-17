print("BEGIN currentop.js");

// test basic currentop functionality + querying of nested documents
t = db.jstests_currentop
t.drop();

for(i=0;i<100;i++) {
    t.save({ "num": i });
}
// Make sure data is written before we start reading it in parallel shells.
db.getLastError();

print("count:" + t.count());

function ops(q) {
    printjson( db.currentOp().inprog );
    return db.currentOp(q).inprog;
}

print("start shell");

// sleep for a second for each (of 100) documents; can be killed in between documents & test should complete before 100 seconds 
s1 = startParallelShell("db.jstests_currentop.count( { '$where': function() { sleep(1000); } } )");

print("sleep");
sleep(1000);

print("inprog:");
printjson(db.currentOp().inprog)
print()
sleep(1);
print("inprog:");
printjson(db.currentOp().inprog)
print()

// need to wait for read to start
print("wait have some ops");
assert.soon( function(){
    return ops( { "locks.^test": "r", "ns": "test.jstests_currentop" } ).length + 
        ops({ "locks.^test": "R", "ns": "test.jstests_currentop" }).length >= 1;
}, "have_some_ops");
print("ok");
    
s2 = startParallelShell( "db.jstests_currentop.update( { '$where': function() { sleep(150); } }, { 'num': 1 }, false, true ); db.getLastError()" );

o = [];

function f() {
    o = ops({ "ns": "test.jstests_currentop" });

    printjson(o);

    var writes = ops({ "locks.^test": "w", "ns": "test.jstests_currentop" }).length;

    var readops = ops({ "locks.^test": "r", "ns": "test.jstests_currentop" });
    print("readops:");
    printjson(readops);
    var reads = readops.length;

    print("total: " + o.length + " w: " + writes + " r:" + reads);

    return o.length > writes && o.length > reads;
}

print("go");

assert.soon( f, "f" );

// avoid waiting for the operations to complete (if soon succeeded)
for(var i in o) {
    db.killOp(o[i].opid);
}

start = new Date();

s1();
s2();

// don't want to pass if timeout killed the js function
assert( ( new Date() ) - start < 30000 );
