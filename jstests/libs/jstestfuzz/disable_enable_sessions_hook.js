// Disable sessions and shim `db` for the duration of the preamble to prevent
// interference from external forces that change the global `db` object.
(() => {
    'use strict';

    load('jstests/libs/jstestfuzz/hook_utils.js');

    let originalDB;

    defineFuzzerHooks({
        beforeServerInfo: function() {
            TestData.disableEnableSessions = true;
            originalDB = db;
            db = db.getMongo().getDB(db.getName());

            // Setting db._session to be a _DummyDriverSession instance makes it so that
            // a logical session id isn't included in the commands the fuzzer runs in the
            // preamble and therefore won't interfere with the session associated with the
            // global "db" object.
            db._session = new _DummyDriverSession(db.getMongo());
        },
        afterServerInfo: function() {
            delete TestData.disableEnableSessions;
            assert(!TestData.hasOwnProperty('disableEnableSessions'),
                   'Unable to delete the "disableEnableSessions" property from TestData');
            db = originalDB;
        },
    });
})();
