// interrupting a count returns an error code

t = db.count10;
t.drop();

for ( i=0; i<100; i++ ){
    t.save( { x : i } );
}
// make sure data is written
db.getLastError();

var thr = new Thread(function () {
    try {
        db.count10.find("sleep(1000)").count();
    }
    catch (e) {
        return e;
    }
});

thr.start();
sleep(1000);

current = db.currentOp({"ns": t.getFullName(), "query.count": t.getName()});
assert(current);
countOp = current.inprog[0];
assert(countOp);

db.killOp(countOp.opid);
res = thr.returnData();

assert(res);
assert(res.match(/count failed/) !== null);
assert(res.match(/\"code\"/) !== null);
