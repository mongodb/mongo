// Both foreground and background index builds can be aborted using killop.  SERVER-3067

t = db.jstests_slownightly_index_killop;
t.drop();

// Insert a large number of documents, enough to ensure that an index build on these documents will
// be interrupted before complete.
var bulk = t.initializeUnorderedBulkOp();
for (i = 0; i < 1e6; ++i) {
    bulk.insert({a: i});
}
assert.writeOK(bulk.execute());

function debug(x) {
    //    printjson( x );
}

/** @return the op id for the running index build, or -1 if there is no current index build. */
function getIndexBuildOpId() {
    inprog = db.currentOp().inprog;
    debug(inprog);
    indexBuildOpId = -1;
    inprog.forEach(function(op) {
        // Identify the index build as the createIndex command
        // It is assumed that no other clients are concurrently
        // accessing the 'test' database.
        if ((op.op == 'query' || op.op == 'command') && 'createIndexes' in op.query) {
            debug(op.opid);
            indexBuildOpId = op.opid;
        }
    });
    return indexBuildOpId;
}

/** Test that building an index with @param 'options' can be aborted using killop. */
function testAbortIndexBuild(options) {
    var createIdx = startParallelShell('var coll = db.jstests_slownightly_index_killop;' +
                                       'assert.commandWorked(coll.createIndex({ a: 1 }, ' +
                                       tojson(options) + '));');

    // When the index build starts, find its op id.
    assert.soon(function() {
        return (opId = getIndexBuildOpId()) != -1;
    });
    // Kill the index build.
    db.killOp(opId);

    // Wait for the index build to stop.
    assert.soon(function() {
        return getIndexBuildOpId() == -1;
    });

    var exitCode = createIdx({checkExitSuccess: false});
    assert.neq(
        0, exitCode, 'expected shell to exit abnormally due to index build being terminated');

    // Check that no new index has been created.  This verifies that the index build was aborted
    // rather than successfully completed.
    assert.eq([{_id: 1}], t.getIndexKeys());
}

testAbortIndexBuild({background: false});
testAbortIndexBuild({background: true});
