t = db.jstests_evalc;
t.drop();

t2 = db.evalc_done;
t2.drop();

for (i = 0; i < 10; ++i) {
    t.save({i: i});
}

// SERVER-1610

assert.eq(0, t2.count(), "X1");

s = startParallelShell(
    "print( 'starting forked:' + Date() ); for ( i=0; i<10*1000; i++ ){ db.currentOp(); } print( 'ending forked:' + Date() ); db.evalc_done.insert( { x : 1 } ); ");

print("starting eval: " + Date());
assert.soon(function() {
    db.eval("db.jstests_evalc.count( {i:10} );");
    return t2.count() > 0;
}, 'parallel shell failed to update ' + t2.getFullName(), 120000, 10);

print("end eval: " + Date());

s();
