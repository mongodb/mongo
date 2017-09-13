// validate command line ftdc parameter parsing

(function() {
    'use strict';
    var m = MongoRunner.runMongod({setParameter: "diagnosticDataCollectionPeriodMillis=101"});

    // Check the defaults are correct
    //
    function getparam(field) {
        var q = {getParameter: 1};
        q[field] = 1;

        var ret = m.getDB("admin").runCommand(q);
        return ret[field];
    }

    assert.eq(getparam("diagnosticDataCollectionPeriodMillis"), 101);
})();
