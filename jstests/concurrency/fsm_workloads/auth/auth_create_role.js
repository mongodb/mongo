/**
 * auth_create_role.js
 *
 * Repeatedly creates new roles on a database.
 *
 * @tags: [
 *  assumes_against_mongod_not_mongos,
 *  requires_auth,
 * ]
 */
import {dropRoles} from "jstests/concurrency/fsm_workload_helpers/drop_utils.js";

// UMC commands are not supported in transactions.
TestData.runInsideTransaction = false;

export const $config = (function () {
    let data = {
        // Use the workload name as a prefix for the role name,
        // since the workload name is assumed to be unique.
        prefix: "auth_create_role",
    };

    let states = (function () {
        function uniqueRoleName(prefix, tid, num) {
            return prefix + tid + "_" + num;
        }

        function init(db, collName) {
            this.num = 0;
        }

        function createRole(db, collName) {
            const roleName = uniqueRoleName(this.prefix, this.tid, this.num++);
            const kCreateRoleRetries = 5;
            const kCreateRoleRetryInterval = 5 * 1000;
            assert.retry(
                function () {
                    try {
                        db.createRole({
                            role: roleName,
                            privileges: [
                                {
                                    resource: {db: db.getName(), collection: collName},
                                    actions: ["update"],
                                },
                            ],
                            roles: [{role: "read", db: db.getName()}],
                        });
                        return true;
                    } catch (e) {
                        jsTest.log("Caught createRole exception: " + tojson(e));
                        return false;
                    }
                },
                "Failed creating role '" + roleName + "'",
                kCreateRoleRetries,
                kCreateRoleRetryInterval,
            );

            // Verify the newly created role exists, as well as all previously created roles
            for (let i = 0; i < this.num; ++i) {
                let name = uniqueRoleName(this.prefix, this.tid, i);
                let res = db.getRole(name);
                assert(res !== null, "role '" + name + "' should exist");
                assert.eq(name, res.role);
                assert(!res.isBuiltin, "role should be user-defined");
            }
        }

        return {init: init, createRole: createRole};
    })();

    let transitions = {init: {createRole: 1}, createRole: {createRole: 1}};

    function teardown(db, collName, cluster) {
        let pattern = new RegExp("^" + this.prefix + "\\d+_\\d+$");
        dropRoles(db, pattern);
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
