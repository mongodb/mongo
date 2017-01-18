var coll = db.jstests_drop2;
coll.drop();

function debug(x) {
    printjson(x);
}

coll.save({});

function getOpId(drop) {
    var inProg = db.currentOp().inprog;
    debug(inProg);
    for (var id in inProg) {
        var op = inProg[id];
        if (drop) {
            if (op.query && op.query.drop && op.query.drop == coll.getName()) {
                return op.opid;
            }
        } else {
            if (op.query && op.query.query && op.query.query.$where && op.ns == (coll + "")) {
                return op.opid;
            }
        }
    }
    return null;
}

var awaitCount = startParallelShell(
    "print(\"Count thread started\");" + "db.getMongo().getCollection(\"" + (coll + "") + "\")" +
    ".count( { $where: function() {" + "while( 1 ) { sleep( 1 ); } } } );" +
    "print(\"Count thread terminating\");");
countOpId = null;
assert.soon(function() {
    countOpId = getOpId(false);
    return countOpId;
});

var awaitDrop =
    startParallelShell("print(\"Drop thread started\");" + "print(\"drop result: \" + " +
                       "db.getMongo().getCollection(\"" + (coll + "") + "\")" + ".drop() );" +
                       "print(\"Drop thread terminating\")");
dropOpId = null;
assert.soon(function() {
    dropOpId = getOpId(true);
    return dropOpId;
});

db.killOp(dropOpId);
db.killOp(countOpId);

var exitCode = awaitCount({checkExitSuccess: false});
assert.neq(0, exitCode, "expected shell to exit abnormally due to JS execution being terminated");

// The drop operation may or may not have been killed.
awaitDrop({checkExitSuccess: false});

coll.drop();  // in SERVER-1818, this fails
