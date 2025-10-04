/**
 * auth_create_user.js
 *
 * Repeatedly creates new users on a database.
 * @tags: [
 *  assumes_against_mongod_not_mongos,
 *  requires_auth,
 * ]
 *
 */
import {dropUsers} from "jstests/concurrency/fsm_workload_helpers/drop_utils.js";

// UMC commands are not supported in transactions.
TestData.runInsideTransaction = false;

export const $config = (function () {
    let data = {
        // Use the workload name as a prefix for the username,
        // since the workload name is assumed to be unique.
        prefix: "auth_create_user",
    };

    let states = (function () {
        function uniqueUsername(prefix, tid, num) {
            return prefix + tid + "_" + num;
        }

        function init(db, collName) {
            this.num = 0;
        }

        function createUser(db, collName) {
            const username = uniqueUsername(this.prefix, this.tid, this.num++);
            const kCreateUserRetries = 5;
            const kCreateUserRetryInterval = 5 * 1000;
            assert.retry(
                function () {
                    try {
                        db.createUser({user: username, pwd: "password", roles: ["readWrite", "dbAdmin"]});
                        return true;
                    } catch (e) {
                        jsTest.log("Caught createUser exception: " + tojson(e));
                        return false;
                    }
                },
                "Failed creating user: '" + username + "'",
                kCreateUserRetries,
                kCreateUserRetryInterval,
            );

            // Verify the newly created user exists, as well as all previously created users
            for (let i = 0; i < this.num; ++i) {
                let res = db.getUser(username);
                assert(res !== null, "user '" + username + "' should exist");
                assert.eq(username, res.user);
                assert.eq(db.getName(), res.db);
            }
        }

        return {init: init, createUser: createUser};
    })();

    let transitions = {init: {createUser: 1}, createUser: {createUser: 1}};

    function teardown(db, collName, cluster) {
        let pattern = new RegExp("^" + this.prefix + "\\d+_\\d+$");
        dropUsers(db, pattern);
    }

    return {
        threadCount: 10,
        iterations: 20,
        data: data,
        states: states,
        transitions: transitions,
        teardown: teardown,
    };
})();
