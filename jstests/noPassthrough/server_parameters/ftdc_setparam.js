// validate command line ftdc parameter parsing

let m = MongoRunner.runMongod({setParameter: "diagnosticDataCollectionPeriodMillis=101"});

// Check the defaults are correct
//
function getparam(field) {
    let q = {getParameter: 1};
    q[field] = 1;

    let ret = m.getDB("admin").runCommand(q);
    return ret[field];
}

assert.eq(getparam("diagnosticDataCollectionPeriodMillis"), 101);
MongoRunner.stopMongod(m);
