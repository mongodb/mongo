'use strict';

/**
 * auth_privilege_consistency.js
 *
 * Validate user cache invalidation upon subordinate role removal.
 *
 * @tags: [requires_fcv_47]
 */
load('jstests/concurrency/fsm_workload_helpers/drop_utils.js');  // for dropRoles

var $config = (function() {
    const kTestNamePrefix = 'auth_privilege_consistency';
    const kTestUserName = kTestNamePrefix + '_user';
    const kTestUserPassword = 'secret';
    const kTestRoleNamePrefix = kTestNamePrefix + '_role_';

    const states = (function() {
        let roleName = kTestRoleNamePrefix;
        let roleWithDB = {};
        let privilege = {actions: ['insert', 'update', 'remove', 'find']};
        let RSnodes = [];

        function getTestUser(node, dbName) {
            const users = assert
                              .commandWorked(node.getDB(dbName).runCommand(
                                  {usersInfo: kTestUserName, showPrivileges: 1}))
                              .users;
            assert.eq(users.length, 1, tojson(users));
            return users[0];
        }

        return {
            init: function(db, collName) {},

            mutateInit: function(db, collName) {
                privilege.resource = {db: db.getName(), collection: ''};
                roleName += this.tid;
                roleWithDB = {role: roleName, db: db.getName()};
                db.createRole({role: roleName, privileges: [privilege], roles: []});
            },

            mutate: function(db, collName) {
                // Revoke privs from intermediate role,
                // then give that, now empty, role to the user.
                db.revokePrivilegesFromRole(roleName, [privilege]);
                db.grantRolesToUser(kTestUserName, [roleWithDB]);

                // Take the role away from the user, and give it privs.
                db.revokeRolesFromUser(kTestUserName, [roleWithDB]);
                db.grantPrivilegesToRole(roleName, [privilege]);
            },

            observeInit: function(db, collName) {
                // Drop privileges to normal user.
                // The workload runner disallows `db.logout()` for reasons we're okay with.
                assert.commandWorked(db.runCommand({logout: 1}));
                assert(db.auth(kTestUserName, kTestUserPassword));

                // Setup a connection to every member host if this is a replica set
                // so that we can confirm secondary state during observe().
                const hello = assert.commandWorked(db.runCommand({hello: 1}));
                jsTest.log('hello: ' + tojson(hello));
                if (hello.hosts) {
                    const allHosts = hello.hosts.concat(hello.passives);
                    allHosts.forEach(function(node) {
                        if (node === hello.me) {
                            // Reuse existing connection for db's connection.
                            const conn = db.getMongo();
                            RSnodes.push(conn);
                            return;
                        }

                        // Create a new connection to any node which isn't "me".
                        const conn = new Mongo(node);
                        assert(conn);
                        conn.setSecondaryOk();
                        RSnodes.push(conn);
                    });

                    // Wait for user to replicate to all nodes.
                    RSnodes.forEach(function(node) {
                        assert.soon(function() {
                            try {
                                getTestUser(node, db.getName());
                                return true;
                            } catch (e) {
                                return false;
                            }
                        });
                    });
                }
            },

            observe: function(db, collName) {
                // Make sure we never appear to have any privileges,
                // but that we remain authenticated.
                const info =
                    assert.commandWorked(db.runCommand({connectionStatus: 1, showPrivileges: true}))
                        .authInfo;
                assert.eq(info.authenticatedUsers.length, 1, tojson(info));
                assert.eq(info.authenticatedUsers[0].user, kTestUserName, tojson(info));
                assert.eq(info.authenticatedUserPrivileges.length, 0, tojson(info));

                // If this is a ReplSet, iterate nodes and check usersInfo.
                RSnodes.forEach(function(node) {
                    const user = getTestUser(node, db.getName());
                    jsTest.log(node + ' userInfo: ' + tojson(user));
                    assert.eq(user.user, kTestUserName, tojson(user));
                    assert.eq(user.inheritedPrivileges.length, 0, tojson(user));
                });
            },
        };
    })();

    const transitions = {
        init: {mutateInit: 0.8, observeInit: 0.2},
        mutateInit: {mutate: 1},
        mutate: {mutate: 1},
        observeInit: {observe: 1},
        observe: {observe: 1},
    };

    function setup(db, collName, cluster) {
        db.createUser({user: kTestUserName, pwd: kTestUserPassword, roles: []});
    }

    function teardown(db, collName, cluster) {
        const pattern = new RegExp('^' + kTestRoleNamePrefix + '\\d+$');
        dropRoles(db, pattern);
        db.dropUser(kTestUserName);
    }

    return {
        threadCount: 50,
        iterations: 10,
        states: states,
        transitions: transitions,
        setup: setup,
        teardown: teardown,
    };
})();
