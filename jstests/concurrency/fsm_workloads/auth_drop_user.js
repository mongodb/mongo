'use strict';

/**
 * auth_drop_user.js
 *
 * Repeatedly creates a new user on a database, and subsequently
 * drops the user from the database.
 */
var $config = (function() {

    var data = {
        // Use the workload name as a prefix for the username,
        // since the workload name is assumed to be unique.
        prefix: 'auth_drop_user'
    };

    var states = (function() {

        function uniqueUsername(prefix, tid, num) {
            return prefix + tid + '_' + num;
        }

        function init(db, collName) {
            this.num = 0;
        }

        function createAndDropUser(db, collName) {
            var username = uniqueUsername(this.prefix, this.tid, this.num++);
            db.createUser({user: username, pwd: 'password', roles: ['readWrite', 'dbAdmin']});

            var res = db.getUser(username);
            assertAlways(res !== null, "user '" + username + "' should exist");
            assertAlways.eq(username, res.user);
            assertAlways.eq(db.getName(), res.db);

            assertAlways(db.dropUser(username));
            assertAlways.isnull(db.getUser(username), "user '" + username + "' should not exist");
        }

        return {init: init, createAndDropUser: createAndDropUser};

    })();

    var transitions = {init: {createAndDropUser: 1}, createAndDropUser: {createAndDropUser: 1}};

    return {threadCount: 10, iterations: 20, data: data, states: states, transitions: transitions};

})();
