// Tests that the default read preference is 'unset', and that the slaveOk bit is not set
// on read commands run with an 'unset' read preference.
(function() {

    "use strict";

    var merizo = db.getMongo();
    try {
        var commandsRan = [];
        db._merizo = {
            getSlaveOk: function() {
                return false;
            },
            getReadPrefMode: function() {
                return merizo.getReadPrefMode();
            },
            getReadPref: function() {
                return merizo.getReadPref();
            },
            runCommand: function(db, cmd, opts) {
                commandsRan.push({db: db, cmd: cmd, opts: opts});
                return {ok: 1};
            },
            getMinWireVersion: function() {
                return merizo.getMinWireVersion();
            },
            getMaxWireVersion: function() {
                return merizo.getMaxWireVersion();
            },
            isReplicaSetMember: function() {
                return merizo.isReplicaSetMember();
            },
            isMongos: function() {
                return merizo.isMongos();
            },
            isCausalConsistency: function() {
                return false;
            },
            getClusterTime: function() {
                return null;
            },
        };
        db._session = new _DummyDriverSession(db._merizo);

        db.runReadCommand({ping: 1});
        assert.eq(commandsRan.length, 1);
        assert.docEq(commandsRan[0].cmd, {ping: 1}, "The command should not have been wrapped.");
        assert.eq(
            commandsRan[0].opts & DBQuery.Option.slaveOk, 0, "The slaveOk bit should not be set.");

    } finally {
        db._merizo = merizo;
        db._session = new _DummyDriverSession(merizo);
    }

})();
