// Test escaping of user provided data in logs
// @tags: [requires_non_retryable_commands]

(function() {
    'use strict';
    load('jstests/libs/check_log.js');

    const mongo = db.getMongo();
    const admin = mongo.getDB('admin');

    // Test a range of characters sent to the global log
    for (let i = 1; i < 256; ++i) {
        const msg = "Hello" + String.fromCharCode(i) + "World";
        assert.commandWorked(admin.runCommand({logMessage: msg}));
        const escmsg = msg.replace("\r", "\\r").replace("\n", "\\n");
        checkLog.contains(mongo, "logMessage: " + escmsg);
    }
})();
