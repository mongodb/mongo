// Test that interrupting a count returns an error code.

t = db.count10;
t.drop();

for ( i=0; i<100; i++ ){
    t.save( { x : i } );
}
// Make sure data is written.
db.getLastError();

// Start a parallel shell which repeatedly checks for a count
// query using db.currentOp(). As soon as the op is found,
// kill it via db.killOp().
s = startParallelShell(
    'assert.soon(function() {' +
    '   current = db.currentOp({"ns": db.count10.getFullName(), ' +
    '                           "query.count": db.count10.getName()}); ' +
    '   if (!current) return false; ' +
    '   countOp = current.inprog[0]; ' +
    '   assert(countOp, "missing countOp"); ' +
    '   db.killOp(countOp.opid); ' +
    '   return true; ' +
    '}, "could not kill count op");'
);

function getKilledCount() {
    try {
        db.count10.find("sleep(1000)").count();
    } catch (e) {
        return e;
    }
}

var res = getKilledCount();
jsTest.log("killed count output start");
printjson(res);
jsTest.log("killed count output end");
assert(res);
assert(res.match(/count failed/) !== null);
assert(res.match(/\"code\"/) !== null);

s();

