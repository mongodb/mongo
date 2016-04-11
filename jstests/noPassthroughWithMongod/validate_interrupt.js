// Tests that the validate command can be interrupted by specifying a low maxTimeMS.

'use strict';

(function() {
    var t = db.validate_interrupt;
    t.drop();

    var bulk = t.initializeUnorderedBulkOp();

    var i;
    for (i = 0; i < 1000; i++) {
        bulk.insert({a: i});
    }
    assert.writeOK(bulk.execute());

    var res = t.runCommand({validate: t.getName(), full: true, maxTimeMS: 1});

    if (res.ok === 0) {
        assert.eq(res.code, ErrorCodes.ExceededTimeLimit, 'validate command did not time out');
    } else {
        // validate() should only succeed if it EBUSY'd. See SERVER-23131.
        var numWarnings = res.warnings.length;
        // validate() could EBUSY when verifying the index and/or the RecordStore, so EBUSY could
        // appear once or twice.
        assert((numWarnings === 1) || (numWarnings === 2), tojson(res));
        assert(res.warnings[0].includes('EBUSY'), tojson(res));
        if (numWarnings === 2) {
            assert(res.warnings[1].includes('EBUSY'), tojson(res));
        }
    }
})();
