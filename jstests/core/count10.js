// Test that interrupting a count returns an error code.

t = db.count10;
t.drop();

for (i = 0; i < 100; i++) {
    t.save({x: i});
}

// Start a parallel shell which repeatedly checks for a count
// query using db.currentOp(). As soon as the op is found,
// kill it via db.killOp().
s = startParallelShell('assert.soon(function() {' +
                       '   current = db.currentOp({"ns": db.count10.getFullName(), ' +
                       '                           "query.count": db.count10.getName()}); ' +

                       // Check that we found the count op. If not, return false so
                       // that assert.soon will retry.
                       '   assert("inprog" in current); ' +
                       '   if (current.inprog.length === 0) { ' +
                       '       jsTest.log("count10.js: did not find count op, retrying"); ' +
                       '       printjson(current); ' +
                       '       return false; ' +
                       '   } ' +
                       '   countOp = current.inprog[0]; ' +
                       '   if (!countOp) { ' +
                       '       jsTest.log("count10.js: did not find count op, retrying"); ' +
                       '       printjson(current); ' +
                       '       return false; ' +
                       '   } ' +

                       // Found the count op. Try to kill it.
                       '   jsTest.log("count10.js: found count op:"); ' +
                       '   printjson(current); ' +
                       '   printjson(db.killOp(countOp.opid)); ' +
                       '   return true; ' +
                       '}, "count10.js: could not find count op after retrying, gave up");');

function getKilledCount() {
    try {
        db.count10.find("sleep(1000)").count();
        jsTest.log("count10.js: count op completed without being killed");
    } catch (e) {
        return e;
    }
}

var res = getKilledCount();
jsTest.log("count10.js: killed count output start");
printjson(res);
jsTest.log("count10.js: killed count output end");
assert(res);
assert(res.message.match(/count failed/) !== null);
assert(res.message.match(/\"code\"/) !== null);

s();
