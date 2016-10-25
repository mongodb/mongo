/**
 * Test that db.eval does not support auth.
 */
(function() {
    'use strict';

    assert.writeOK(db.evalprep.insert({}), "db must exist for eval to succeed");
    db.evalprep.drop();

    // The db.auth method call getMongo().auth but catches the exception.
    assert.eq(0, db.eval('db.auth("reader", "reader")'));

    // Call the native implementation auth function and verify it does not exist under the db.eval
    // javascript context.
    assert.throws(function() {
        db.eval('db.getMongo().auth("reader", "reader")');
    });
})();
