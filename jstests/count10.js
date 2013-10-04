// interrupting a count returns an error code

t = db.count10;
t.drop();

for ( i=0; i<100; i++ ){
    t.save( { x : i } );
}
// make sure data is written
db.getLastError();

s = startParallelShell(
    'sleep(1000); ' +
    'current = db.currentOp({"ns": db.count10.getFullName(), "query.count": db.count10.getName()}); ' +
    'assert(current); ' +
    'countOp = current.inprog[0]; ' +
    'assert(countOp); ' +
    'db.killOp(countOp.opid); '
);

function getKilledCount() {
    try {
        db.count10.find("sleep(1000)").count();
    } catch (e) {
        return e;
    }
}

var res = getKilledCount();
assert(res);
assert(res.match(/count failed/) !== null);
assert(res.match(/\"code\"/) !== null);

s();

