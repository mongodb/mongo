'use strict';

/**
 * auth_drop_role.js
 *
 * Repeatedly creates a new role on a database, and subsequently
 * drops it from the database.
 */
load('jstests/concurrency/fsm_workload_helpers/drop_utils.js');  // for dropRoles

var $config = (function() {
    const kMaxCmdTimeMs = 60000;
    const kMaxTxnLockReqTimeMs = 100;
    const kDefaultTxnLockReqTimeMs = 5;

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

            db.runCommand({
                createRole: roleName,
                privileges:
                    [{resource: {db: db.getName(), collection: collName}, actions: ['remove']}],
                roles: [{role: 'read', db: db.getName()}],
                maxTimeMS: kMaxCmdTimeMs
            });

            var res = db.getRole(roleName);

            assertAlways(res !== null, "role '" + roleName + "' should exist");
            assertAlways.eq(roleName, res.role);
            assertAlways(!res.isBuiltin, 'role should be user-defined');

            // Some test machines may hit high contention during these concurrency tests
            // allow for occaisional failure with retries.
            for (var i = 5; i >= 0; --i) {
                let cmdResult;
                try {
                    cmdResult = db.runCommand({dropRole: roleName, maxTimeMS: kMaxCmdTimeMs});
                    assert.commandWorked(cmdResult);
                    break;
                } catch (e) {
                    if (i > 0) {
                        // Failure, try again
                        print("Retrying dropRole(" + roleName + "), previous call resulted in " +
                              tojson(cmdResult));
                        if (cmdResult.code == ErrorCodes.SnapshotUnavailable) {
                            // Give pending catalog changes a chance to catch up.
                            sleep(5000);
                        }
                    } else {
                        // Out of do-overs, just die.
                        throw e;
                    }
                }
            }

            assertAlways.isnull(db.getRole(roleName), "role '" + roleName + "' should not exist");
        }

        return {init: init, createAndDropRole: createAndDropRole};
    })();

    var transitions = {init: {createAndDropRole: 1}, createAndDropRole: {createAndDropRole: 1}};

    function setup(db, collName, cluster) {
        cluster.executeOnMongodNodes(function(db) {
            db.adminCommand(
                {setParameter: 1, maxTransactionLockRequestTimeoutMillis: kMaxTxnLockReqTimeMs});
        });

        cluster.executeOnMongosNodes(function(db) {
            db.adminCommand(
                {setParameter: 1, maxTransactionLockRequestTimeoutMillis: kMaxTxnLockReqTimeMs});
        });
    }

    function teardown(db, collName, cluster) {
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
        threadCount: 10,
        iterations: 20,
        data: data,
        states: states,
        transitions: transitions,
        setup: setup,
        teardown: teardown
    };
})();
