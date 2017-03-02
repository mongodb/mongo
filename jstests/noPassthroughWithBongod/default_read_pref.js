// Tests that the default read preference is 'unset', and that the slaveOk bit is not set
// on read commands run with an 'unset' read preference.
(function() {

    "use strict";

    var bongo = db.getBongo();
    try {
        var commandsRan = [];
        db._bongo = {
            getSlaveOk: function() {
                return false;
            },
            getReadPrefMode: function() {
                return bongo.getReadPrefMode();
            },
            getReadPref: function() {
                return bongo.getReadPref();
            },
            runCommand: function(db, cmd, opts) {
                commandsRan.push({db: db, cmd: cmd, opts: opts});
            }
        };

        db.runReadCommand({ping: 1});
        assert.eq(commandsRan.length, 1);
        assert.docEq(commandsRan[0].cmd, {ping: 1}, "The command should not have been wrapped.");
        assert.eq(
            commandsRan[0].opts & DBQuery.Option.slaveOk, 0, "The slaveOk bit should not be set.");

    } finally {
        db._bongo = bongo;
    }

})();
