//
// Test that we neither rollback logOp nor crash when a WriteUnitOfWork attempts to rollback
//
(function() {
    'use strict';

    function checkForLogOpRollback(coll) {
        var res = coll.runCommand({ getLog: 'global' });
        assert.commandWorked(res);

        for (var i = res.log.length - 1; i >= 0; i--) {
            if (/custom rollback.*logop/i.test(res.log[i])) {
                return true;
            }
        }
        return false;
    }

    var coll = db.getSiblingDB('admin').system.roles;

    //
    // SERVER-17250 -- logOp rollback after triggering a
    // "key too large to index" exception
    //
    var prevVerbosityLevel = db.getLogComponents().verbosity;
    var prevWriteMode = db.getMongo().writeMode();
    try {
        // we need a verbosity level of at least 2 to see rollbacks in the logs
        db.setLogLevel(2);

        // must be in 'legacy' or 'compatibility' mode
        db.getMongo().forceWriteMode('compatibility');
        var res = coll.insert({ _id: new Array(1025).join('x') });

        assert(res.hasWriteError());
        // ErrorCodes::KeyTooLong == 17280
        assert.eq(17280, res.getWriteError().code);

        assert(checkForLogOpRollback(coll));
    }
    finally {
        db.getMongo().forceWriteMode(prevWriteMode);
        db.setLogLevel(prevVerbosityLevel);
    }

})();
