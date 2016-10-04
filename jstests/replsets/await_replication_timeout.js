// Tests timeout behavior of waiting for write concern as well as its interaction with maxTimeMs

(function() {
    "use strict";

    var exceededTimeLimitCode = 50;
    var writeConcernFailedCode = 64;
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
    assert.commandWorked(res);  // Commands with write concern errors still report success.
    assert.eq(writeConcernFailedCode, res.writeConcernError.code);

    // Test maxTimeMS timeout
    res = testDB.runCommand(
        {insert: 'foo', documents: [{a: 1}], writeConcern: {w: 3}, maxTimeMS: 1000});
    assert.commandWorked(res);  // Commands with write concern errors still report success.
    assert.eq(exceededTimeLimitCode, res.writeConcernError.code);

    // Test with wtimeout < maxTimeMS
    res = testDB.runCommand({
        insert: 'foo',
        documents: [{a: 1}],
        writeConcern: {w: 3, wtimeout: 1000},
        maxTimeMS: 10 * 1000
    });
    assert.commandWorked(res);  // Commands with write concern errors still report success.
    assert.eq(writeConcernFailedCode, res.writeConcernError.code);

    // Test with wtimeout > maxTimeMS
    res = testDB.runCommand({
        insert: 'foo',
        documents: [{a: 1}],
        writeConcern: {w: 3, wtimeout: 10 * 1000},
        maxTimeMS: 1000
    });
    assert.commandWorked(res);  // Commands with write concern errors still report success.
    assert.eq(exceededTimeLimitCode, res.writeConcernError.code);
    replTest.stopSet();

})();
