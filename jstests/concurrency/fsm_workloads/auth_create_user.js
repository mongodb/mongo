'use strict';

/**
 * auth_create_user.js
 *
 * Repeatedly creates new users on a database.
 */
load('jstests/concurrency/fsm_workload_helpers/drop_utils.js');  // for dropUsers

var $config = (function() {

    var data = {
        // Use the workload name as a prefix for the username,
        // since the workload name is assumed to be unique.
        prefix: 'auth_create_user'
    };

    var states = (function() {

        function uniqueUsername(prefix, tid, num) {
            return prefix + tid + '_' + num;
        }

        function init(db, collName) {
            this.num = 0;
        }

        function createUser(db, collName) {
            var username = uniqueUsername(this.prefix, this.tid, this.num++);
            db.createUser({user: username, pwd: 'password', roles: ['readWrite', 'dbAdmin']});

            // Verify the newly created user exists, as well as all previously created users
            for (var i = 0; i < this.num; ++i) {
                var name = uniqueUsername(this.prefix, this.tid, i);
                var res = db.getUser(username);
                assertAlways(res !== null, "user '" + username + "' should exist");
                assertAlways.eq(username, res.user);
                assertAlways.eq(db.getName(), res.db);
            }
        }

        return {init: init, createUser: createUser};

    })();

    var transitions = {init: {createUser: 1}, createUser: {createUser: 1}};

    function teardown(db, collName, cluster) {
        var pattern = new RegExp('^' + this.prefix + '\\d+_\\d+$');
        dropUsers(db, pattern);
    }

    return {
        threadCount: 10,
        iterations: 20,
        data: data,
        states: states,
        transitions: transitions,
        teardown: teardown
    };

})();
