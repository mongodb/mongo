// SERVER-18198 check read pref is only applied when there is no $out stage
// in aggregate shell helper
(function() {
    "use strict";
    var t = db.server18198;
    t.drop();

    var merizo = db.getMerizo();

    try {
        var commandsRan = [];
        // hook in our patched merizo
        var mockMerizo = {
            getSlaveOk: function() {
                return true;
            },
            runCommand: function(db, cmd, opts) {
                commandsRan.push({db: db, cmd: cmd, opts: opts});
                return {ok: 1.0};
            },
            getReadPref: function() {
                return {mode: "secondaryPreferred"};
            },
            getReadPrefMode: function() {
                return "secondaryPreferred";
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
            isMerizos: function() {
                return merizo.isMerizos();
            },
            isCausalConsistency: function() {
                return false;
            },
            getClusterTime: function() {
                return merizo.getClusterTime();
            },
        };

        db._merizo = mockMerizo;
        db._session = new _DummyDriverSession(mockMerizo);

        // this query should not get a read pref
        t.aggregate([{$sort: {"x": 1}}, {$out: "foo"}]);
        assert.eq(commandsRan.length, 1);
        // check that it doesn't have a read preference
        assert(!commandsRan[0].cmd.hasOwnProperty("$readPreference"));

        commandsRan = [];

        t.aggregate([{$sort: {"x": 1}}]);
        // check another command was run
        assert.eq(commandsRan.length, 1);
        // check that it has a read preference
        assert(commandsRan[0].cmd.hasOwnProperty("$readPreference"));
    } finally {
        db._merizo = merizo;
        db._session = new _DummyDriverSession(merizo);
    }
})();
