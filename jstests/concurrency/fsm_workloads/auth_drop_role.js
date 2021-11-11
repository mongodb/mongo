'use strict';

/**
 * auth_drop_role.js
 *
 * Repeatedly creates a new role on a database, and subsequently
 * drops it from the database.
 */
load('jstests/concurrency/fsm_workload_helpers/drop_utils.js');  // for dropRoles

var $config = (function() {
    var data = {
        // Use the workload name as a prefix for the role name,
        // since the workload name is assumed to be unique.
        prefix: 'auth_drop_role'
    };

    var states = (function() {
        function uniqueRoleName(prefix, tid, num) {
            return prefix + tid + '_' + num;
        }

        function init(db, collName) {
            this.num = 0;
        }

        function createAndDropRole(db, collName) {
            var roleName = uniqueRoleName(this.prefix, this.tid, this.num++);
            db.createRole({
                role: roleName,
                privileges:
                    [{resource: {db: db.getName(), collection: collName}, actions: ['remove']}],
                roles: [{role: 'read', db: db.getName()}]
            });

            var res = db.getRole(roleName);
            assertAlways(res !== null, "role '" + roleName + "' should exist");
            assertAlways.eq(roleName, res.role);
            assertAlways(!res.isBuiltin, 'role should be user-defined');

            // Some test machines may hit high contention during these concurrency tests
            // allow for occaisional failure with retries.
            for (var i = 3; i >= 0; --i) {
                var dropResult = db.dropRole(roleName);
                if (dropResult === true) {
                    // Success
                    break;
                } else if (i > 0) {
                    // Failure, try again
                    print("Retrying a dropRole() which resulted in: " + tojson(dropResult));
                } else {
                    // Out of do-overs, just die.
                    assertAlways(dropResult);
                }
            }

            assertAlways.isnull(db.getRole(roleName), "role '" + roleName + "' should not exist");
        }

        return {init: init, createAndDropRole: createAndDropRole};
    })();

    var transitions = {init: {createAndDropRole: 1}, createAndDropRole: {createAndDropRole: 1}};

    return {threadCount: 10, iterations: 20, data: data, states: states, transitions: transitions};
})();
