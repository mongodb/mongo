/**
 * Test that a user may currentOp, and then killOp their own operations.
 *
 * Theory of operation: Create a long running operation from a user which does not have the killOp
 * or inProg privileges. Using the same user, run currentOp to get the opId, and then run killOp
 * against it.
 */

(function() {
    'use strict';

    function runTest(m) {
        var db = m.getDB("foo");
        var admin = m.getDB("admin");

        admin.createUser({user: 'admin', pwd: 'password', roles: jsTest.adminUserRoles});
        admin.auth('admin', 'password');
        db.createUser({user: 'reader', pwd: 'reader', roles: [{db: 'foo', role: 'read'}]});
        db.createUser(
            {user: 'otherReader', pwd: 'otherReader', roles: [{db: 'foo', role: 'read'}]});
        admin.createRole({
            role: 'opAdmin',
            roles: [],
            privileges: [{resource: {cluster: true}, actions: ['inprog', 'killop']}]
        });
        db.createUser({user: 'opAdmin', pwd: 'opAdmin', roles: [{role: 'opAdmin', db: 'admin'}]});

        var t = db.jstests_killop;
        t.save({x: 1});

        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1}));

        admin.logout();

        /**
         * This function filters for the operations that we're looking for, based on their state and
         * the contents of their query object.
         */
        function ops(ownOps = true) {
            var p = db.currentOp({$ownOps: ownOps}).inprog;
            var ids = [];
            for (var i in p) {
                var o = p[i];
                // We *can't* check for ns, b/c it's not guaranteed to be there unless the query is
                // active, which it may not be in our polling cycle - particularly b/c we sleep
                // every
                // second in both the query and the assert
                if ((o.active || o.waitingForLock) && o.query && o.query &&
                    o.query.find === "jstests_killop" && o.query.comment === "kill_own_ops") {
                    print("OP: " + tojson(o));
                    ids.push(o.opid);
                }
            }
            return ids;
        }

        var queryAsReader =
            'db = db.getSiblingDB("foo"); db.auth("reader", "reader"); db.jstests_killop.find().comment("kill_own_ops").toArray()';

        jsTestLog("Starting long-running operation");
        db.auth('reader', 'reader');
        assert.commandWorked(
            db.adminCommand({configureFailPoint: "setYieldAllLocksHang", mode: "alwaysOn"}));
        var s1 = startParallelShell(queryAsReader, m.port);

        jsTestLog("Finding ops in currentOp() output");
        var o = [];
        assert.soon(
            function() {
                o = ops();
                return o.length == 1;
            },
            {
              toString: function() {
                  return tojson(db.currentOp().inprog);
              }
            },
            60000);

        jsTestLog("Checking that another user cannot see or kill the op");
        db.logout();
        db.auth('otherReader', 'otherReader');
        assert.eq([], ops());
        assert.commandFailed(db.killOp(o[0]));
        db.logout();
        db.auth('reader', 'reader');
        assert.eq(1, ops().length);
        db.logout();

        jsTestLog("Checking that originating user can kill operation");
        var start = new Date();
        db.auth('reader', 'reader');
        assert.commandWorked(db.killOp(o[0]));
        assert.commandWorked(
            db.adminCommand({configureFailPoint: "setYieldAllLocksHang", mode: "off"}));

        jsTestLog("Waiting for ops to terminate");
        var exitCode = s1({checkExitSuccess: false});
        assert.neq(0,
                   exitCode,
                   "expected shell to exit abnormally due to operation execution being terminated");

        // don't want to pass if timeout killed the js function.
        var end = new Date();
        var diff = end - start;
        assert.lt(diff, 30000, "Start: " + start + "; end: " + end + "; diff: " + diff);

        jsTestLog("Starting a second long-running operation");
        assert.commandWorked(
            db.adminCommand({configureFailPoint: "setYieldAllLocksHang", mode: "alwaysOn"}));
        var s2 = startParallelShell(queryAsReader, m.port);
        jsTestLog("Finding ops in currentOp() output");
        var o2 = [];
        assert.soon(
            function() {
                o2 = ops();
                return o2.length == 1;
            },
            {
              toString: function() {
                  return tojson(db.currentOp().inprog);
              }
            },
            60000);

        db.logout();
        db.auth('opAdmin', 'opAdmin');

        jsTestLog("Checking that an administrative user can find others' operations");
        assert.eq(o2, ops(false));

        jsTestLog(
            "Checking that an administrative user cannot find others' operations with ownOps");
        assert.eq([], ops());

        jsTestLog("Checking that an administrative user can kill others' operations");
        var start = new Date();
        assert.commandWorked(db.killOp(o2[0]));
        assert.commandWorked(
            db.adminCommand({configureFailPoint: "setYieldAllLocksHang", mode: "off"}));
        jsTestLog("Waiting for ops to terminate");
        var exitCode = s2({checkExitSuccess: false});
        assert.neq(
            0, exitCode, "expected shell to exit abnormally due to JS execution being terminated");

        var end = new Date();
        var diff = end - start;
        assert.lt(diff, 30000, "Start: " + start + "; end: " + end + "; diff: " + diff);
    }

    var m = MongoRunner.runMongod({auth: ""});
    runTest(m);
    MongoRunner.stopMongod(m);

    // TODO: This feature is currently not supported on sharded clusters.
    /*var st =
        new ShardingTest({shards: 2, config: 3, keyFile: 'jstests/libs/key1', useHostname: false});
    runTest(st.s);
    st.stop();*/
})();
