'use strict';

/**
 * auth_privilege_consistency.js
 *
 * Validate user cache invalidation upon subordinate role removal.
 *
 * @tags: [
 * ]
 */
load('jstests/concurrency/fsm_workload_helpers/drop_utils.js');  // for dropRoles

// UMC commands are not supported in transactions.
TestData.runInsideTransaction = false;

var $config = (function() {
    const kTestUserPassword = 'secret';
    const kMaxCmdTimeMs = 60000;
    const kMaxTxnLockReqTimeMs = 100;
    const kDefaultTxnLockReqTimeMs = 5;

    function doRetry(cb) {
        const kNumRetries = 5;
        const kRetryInterval = 5 * 1000;

        assert.retry(function() {
            try {
                cb();
                return true;
            } catch (e) {
                jsTest.log("Caught exception performing: " + tojson(cb) +
                           ", exception was: " + tojson(e));
                return false;
            }
        }, "Failed performing: " + tojson(cb), kNumRetries, kRetryInterval);
    }

    const states = (function() {
        let roleWithDB = {};
        let privilege = {actions: ['insert', 'update', 'remove', 'find']};
        let RSnodes = [];

        function getTestUser(node, userName) {
            const users =
                assert
                    .commandWorked(node.getDB(userName.db)
                                       .runCommand({usersInfo: userName.user, showPrivileges: 1}))
                    .users;
            assert.eq(users.length, 1, tojson(users));
            return users[0];
        }

        return {
            init: function(db, collName) {},

            mutateInit: function(db, collName) {
                privilege.resource = {db: db.getName(), collection: ''};
                const roleName = this.getRoleName(this.tid);
                roleWithDB = {role: roleName, db: db.getName()};
                doRetry(() => db.createRole({role: roleName, privileges: [privilege], roles: []}));
            },

            mutate: function(db, collName) {
                // Revoke privs from intermediate role,
                // then give that, now empty, role to the user.
                const roleName = this.getRoleName(this.tid);

                doRetry(() => assert.commandWorked(db.runCommand({
                    revokePrivilegesFromRole: roleName,
                    privileges: [privilege],
                    maxTimeMS: kMaxCmdTimeMs
                })));

                doRetry(() => assert.commandWorked(db.runCommand({
                    grantRolesToUser: this.getUserName(),
                    roles: [roleWithDB],
                    maxTimeMS: kMaxCmdTimeMs
                })));

                // Take the role away from the user, and give it privs.

                doRetry(() => assert.commandWorked(db.runCommand({
                    revokeRolesFromUser: this.getUserName(),
                    roles: [roleWithDB],
                    maxTimeMS: kMaxCmdTimeMs
                })));

                doRetry(() => assert.commandWorked(db.runCommand({
                    grantPrivilegesToRole: roleName,
                    privileges: [privilege],
                    maxTimeMS: kMaxCmdTimeMs
                })));
            },

            observeInit: function(db, collName) {
                // Drop privileges to normal user.
                // The workload runner disallows `db.logout()` for reasons we're okay with.
                assert.commandWorked(db.runCommand({logout: 1}));
                assert(db.auth(this.getUserName(), kTestUserPassword));

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
                    const userName = {user: this.getUserName(), db: db.getName()};
                    RSnodes.forEach(function(node) {
                        assert.soon(function() {
                            try {
                                getTestUser(node, userName);
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
                assert.eq(info.authenticatedUsers[0].user, this.getUserName(), tojson(info));
                assert.eq(info.authenticatedUserPrivileges.length, 0, tojson(info));

                // If this is a ReplSet, iterate nodes and check usersInfo.
                const userName = {user: this.getUserName(), db: db.getName()};
                RSnodes.forEach(function(node) {
                    const user = getTestUser(node, userName);
                    jsTest.log(node + ' userInfo: ' + tojson(user));
                    assert.eq(user.user, user.user, tojson(user));
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
        cluster.executeOnMongodNodes(function(db) {
            db.adminCommand(
                {setParameter: 1, maxTransactionLockRequestTimeoutMillis: kMaxTxnLockReqTimeMs});
        });

        cluster.executeOnMongosNodes(function(db) {
            db.adminCommand(
                {setParameter: 1, maxTransactionLockRequestTimeoutMillis: kMaxTxnLockReqTimeMs});
        });

        db.createUser({user: this.getUserName(), pwd: kTestUserPassword, roles: []});
    }

    function teardown(db, collName, cluster) {
        // Calling getRoleName() with an empty string allows us to just get the prefix
        // and match any thread id by pattern.
        const pattern = new RegExp('^' + this.getRoleName('') + '\\d+$');
        dropRoles(db, pattern);
        db.dropUser(this.getUserName());

        cluster.executeOnMongodNodes(function(db) {
            db.adminCommand({
                setParameter: 1,
                maxTransactionLockRequestTimeoutMillis: kDefaultTxnLockReqTimeMs
            });
        });

        cluster.executeOnMongosNodes(function(db) {
            db.adminCommand({
                setParameter: 1,
                maxTransactionLockRequestTimeoutMillis: kDefaultTxnLockReqTimeMs
            });
        });
    }

    return {
        threadCount: 50,
        iterations: 10,
        states: states,
        transitions: transitions,
        setup: setup,
        teardown: teardown,
        data: {
            // Tests which extend this must provide their own unique name.
            // So that 'simultaneous' runs will not step over each other.
            test_name: 'auth_privilege_consistency',

            getUserName: function() {
                return this.test_name + '_user';
            },

            getRoleName: function(id) {
                return this.test_name + '_role_' + id;
            },
        },
    };
})();
