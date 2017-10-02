load("jstests/libs/kill_sessions.js");

(function() {
    'use strict';

    function runTests(needAuth) {
        var other = {
            rs: true,
            rsOptions: {nojournal: ""},
            rs0: {nodes: 3},
            rs1: {nodes: 3},
        };
        if (needAuth) {
            other.keyFile = 'jstests/libs/key1';
        }

        var st = new ShardingTest({shards: 2, mongos: 1, config: 1, other: other});

        var forExec = st.s0;

        if (needAuth) {
            KillSessionsTestHelper.initializeAuth(forExec);
        }

        var forKill = new Mongo(forExec.host);

        var r = forExec.getDB("admin").runCommand({
            multicast: {ping: 1},
            db: "admin",
        });
        assert(r.ok);

        var hosts = [];
        for (var host in r["hosts"]) {
            var host = new Mongo(host);
            if (needAuth) {
                host.getDB("local").auth("__system", "foopdedoop");
            }
            hosts.push(host);

            assert.soon(function() {
                var fcv = host.getDB("admin").runCommand(
                    {getParameter: 1, featureCompatibilityVersion: 1});
                return fcv["ok"] && fcv["featureCompatibilityVersion"] != "3.4";
            });
        }

        var args = [forExec, forKill, hosts];
        if (needAuth) {
            KillSessionsTestHelper.runAuth.apply({}, args);
        } else {
            KillSessionsTestHelper.runNoAuth.apply({}, args);
        }

        st.stop();
    }

    runTests(true);
    runTests(false);
})();
