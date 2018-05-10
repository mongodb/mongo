// Tests timeout behavior of waiting for write concern as well as its interaction with maxTimeMs

(function() {
    "use strict";

    var replTest = new ReplSetTest({nodes: 3});
    replTest.startSet();
    replTest.initiate();
    replTest.stop(
        0);  // Make sure that there are only 2 nodes up so w:3 writes will always time out
    var primary = replTest.getPrimary();
    var testDB = primary.getDB('test');

    // Test wtimeout
    var res = testDB.runCommand(
        {insert: 'foo', documents: [{a: 1}], writeConcern: {w: 3, wtimeout: 1000}});
    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernFailed);
    assert.eq(ErrorCodes.WriteConcernFailed, res.writeConcernError.code);

    // Test maxTimeMS timeout
    res = testDB.runCommand(
        {insert: 'foo', documents: [{a: 1}], writeConcern: {w: 3}, maxTimeMS: 1000});
    assert.commandFailedWithCode(res, ErrorCodes.ExceededTimeLimit);
    assert.eq(ErrorCodes.ExceededTimeLimit, res.writeConcernError.code);

    // Test with wtimeout < maxTimeMS
    res = testDB.runCommand({
        insert: 'foo',
        documents: [{a: 1}],
        writeConcern: {w: 3, wtimeout: 1000},
        maxTimeMS: 10 * 1000
    });
    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernFailed);
    assert.eq(ErrorCodes.WriteConcernFailed, res.writeConcernError.code);

    // Test with wtimeout > maxTimeMS
    res = testDB.runCommand({
        insert: 'foo',
        documents: [{a: 1}],
        writeConcern: {w: 3, wtimeout: 10 * 1000},
        maxTimeMS: 1000
    });
    assert.commandFailedWithCode(res, ErrorCodes.ExceededTimeLimit);
    assert.eq(ErrorCodes.ExceededTimeLimit, res.writeConcernError.code);
    replTest.stopSet();

})();
