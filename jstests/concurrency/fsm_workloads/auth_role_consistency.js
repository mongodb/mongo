'use strict';

/**
 * auth_role_consistency.js
 *
 * Add/revoke roles to/from other roles checking for cycles.
 */
load('jstests/concurrency/fsm_workload_helpers/drop_utils.js');  // for dropRoles

var $config = (function() {
    const kRoleNamePrefix = 'auth_role_consistency';

    const states = (function() {
        let roleA = kRoleNamePrefix + '_A_';
        let roleB = kRoleNamePrefix + '_B_';
        let roleAwDB = {};
        let roleBwDB = {};

        // Initial empty A/B roles.
        function init(db, collName) {
            roleA += this.tid;
            roleB += this.tid;
            roleAwDB = {role: roleA, db: db.getName()};
            roleBwDB = {role: roleB, db: db.getName()};

            db.createRole({role: roleA, privileges: [], roles: []});
            db.createRole({role: roleB, privileges: [], roles: []});
        }

        function shuffle(db, collName) {
            // Add A to B, then revoke it.
            db.grantRolesToRole(roleB, [roleAwDB]);
            db.revokeRolesFromRole(roleB, [roleAwDB]);

            // Add B to A, then revoke it.
            // Misordered applications will fassert.
            db.grantRolesToRole(roleA, [roleBwDB]);
            db.revokeRolesFromRole(roleA, [roleBwDB]);
        }

        return {init: init, shuffle: shuffle};
    })();

    function teardown(db, collName, cluster) {
        const pattern = new RegExp('^' + kRoleNamePrefix + '_[AB]_\\d+$');
        dropRoles(db, pattern);
    }

    return {
        threadCount: 50,
        iterations: 10,
        data: {},
        states: states,
        transitions: {init: {shuffle: 1}, shuffle: {shuffle: 1}},
        teardown: teardown,
    };
})();
