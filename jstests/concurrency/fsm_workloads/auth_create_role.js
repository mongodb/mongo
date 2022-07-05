'use strict';

/**
 * auth_create_role.js
 *
 * Repeatedly creates new roles on a database.
 */
load('jstests/concurrency/fsm_workload_helpers/drop_utils.js');  // for dropRoles

// UMC commands are not supported in transactions.
TestData.runInsideTransaction = false;

var $config = (function() {
    var data = {
        // Use the workload name as a prefix for the role name,
        // since the workload name is assumed to be unique.
        prefix: 'auth_create_role'
    };

    var states = (function() {
        function uniqueRoleName(prefix, tid, num) {
            return prefix + tid + '_' + num;
        }

        function init(db, collName) {
            this.num = 0;
        }

        function createRole(db, collName) {
            const roleName = uniqueRoleName(this.prefix, this.tid, this.num++);
            const kCreateRoleRetries = 5;
            const kCreateRoleRetryInterval = 5 * 1000;
            assert.retry(
                function() {
                    try {
                        db.createRole({
                            role: roleName,
                            privileges: [{
                                resource: {db: db.getName(), collection: collName},
                                actions: ['update']
                            }],
                            roles: [{role: 'read', db: db.getName()}]
                        });
                        return true;
                    } catch (e) {
                        jsTest.log("Caught createRole exception: " + tojson(e));
                        return false;
                    }
                },
                "Failed creating role '" + roleName + "'",
                kCreateRoleRetries,
                kCreateRoleRetryInterval);

            // Verify the newly created role exists, as well as all previously created roles
            for (var i = 0; i < this.num; ++i) {
                var name = uniqueRoleName(this.prefix, this.tid, i);
                var res = db.getRole(name);
                assertAlways(res !== null, "role '" + name + "' should exist");
                assertAlways.eq(name, res.role);
                assertAlways(!res.isBuiltin, 'role should be user-defined');
            }
        }

        return {init: init, createRole: createRole};
    })();

    var transitions = {init: {createRole: 1}, createRole: {createRole: 1}};

    function teardown(db, collName, cluster) {
        var pattern = new RegExp('^' + this.prefix + '\\d+_\\d+$');
        dropRoles(db, pattern);
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
