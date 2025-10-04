// UMC commands are not supported in transactions.
TestData.runInsideTransaction = false;

/**
 * auth_drop_user.js
 *
 * Repeatedly creates a new user on a database, and subsequently
 * drops the user from the database.
 * @tags: [
 *  incompatible_with_concurrency_simultaneous,
 *  assumes_against_mongod_not_mongos,
 *  requires_auth,
 * ]
 */

export const $config = (function () {
    let data = {
        // Use the workload name as a prefix for the username,
        // since the workload name is assumed to be unique.
        prefix: "auth_drop_user",
    };

    let states = (function () {
        function uniqueUsername(prefix, tid, num) {
            return prefix + tid + "_" + num;
        }

        function init(db, collName) {
            this.num = 0;
        }

        function createAndDropUser(db, collName) {
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
                "Failed creating user '" + username + "'",
                kCreateUserRetries,
                kCreateUserRetryInterval,
            );

            const res = db.getUser(username);
            assert(res !== null, "user '" + username + "' should exist");
            assert.eq(username, res.user);
            assert.eq(db.getName(), res.db);

            const kDropUserRetries = 5;
            const kDropUserRetryInterval = 5 * 1000;
            assert.retry(
                function () {
                    try {
                        db.dropUser(username);
                        return true;
                    } catch (e) {
                        jsTest.log("Caught dropUser exception: " + tojson(e));
                        return false;
                    }
                },
                "Failed dropping user '" + username + "'",
                kDropUserRetries,
                kDropUserRetryInterval,
            );
            assert.isnull(db.getUser(username), "user '" + username + "' should not exist");
        }

        return {init: init, createAndDropUser: createAndDropUser};
    })();

    let transitions = {init: {createAndDropUser: 1}, createAndDropUser: {createAndDropUser: 1}};

    return {threadCount: 10, iterations: 20, data: data, states: states, transitions: transitions};
})();
