// Returns the op id for the running index build, or -1 if there is no current index build.
function getIndexBuildOpId(db) {
    const result = db.currentOp();
    assert.commandWorked(result);
    let indexBuildOpId = -1;

    result.inprog.forEach(function(op) {
        if (op.op == 'command' && op.query != undefined && 'createIndexes' in op.query) {
            indexBuildOpId = op.opid;
        }
    });
    return indexBuildOpId;
}
