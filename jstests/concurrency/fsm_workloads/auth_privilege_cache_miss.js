'use strict';

/**
 * auth_privilege_cache_miss.js
 *
 * Validate user permission consistency during cache miss and slow load.
 *
 * @tags: [requires_fcv_47]
 */

// Use the auth_privilege_consistency workload as a base.
load('jstests/concurrency/fsm_libs/extend_workload.js');
load('jstests/concurrency/fsm_workloads/auth_privilege_consistency.js');

var $config = extendWorkload($config, function($config, $super) {
    // Override setup() to also set cache-miss and slow load failpoints.
    const kResolveRolesDelayMS = 100;

    const originalSetup = $config.setup;
    const originalTeardown = $config.teardown;

    $config.setup = function(db, collName, cluster) {
        originalSetup(db, collName, cluster);

        const cacheBypass = {configureFailPoint: 'authUserCacheBypass', mode: 'alwaysOn'};
        const getUser = {
            configureFailPoint: 'authLocalGetUser',
            mode: 'alwaysOn',
            data: {resolveRolesDelayMS: NumberInt(kResolveRolesDelayMS)}
        };

        cluster.executeOnMongosNodes(function(nodeAdminDB) {
            assert.commandWorked(nodeAdminDB.runCommand(cacheBypass));
        });

        cluster.executeOnMongodNodes(function(nodeAdminDB) {
            assert.commandWorked(nodeAdminDB.runCommand(cacheBypass));
            assert.commandWorked(nodeAdminDB.runCommand(getUser));
        });
    };

    $config.teardown = function(db, collName, cluster) {
        const cacheBypass = {configureFailPoint: 'authUserCacheBypass', mode: 'off'};
        const getUser = {configureFailPoint: 'authLocalGetUser', mode: 'off'};

        cluster.executeOnMongosNodes(function(nodeAdminDB) {
            assert.commandWorked(nodeAdminDB.runCommand(cacheBypass));
        });

        cluster.executeOnMongodNodes(function(nodeAdminDB) {
            assert.commandWorked(nodeAdminDB.runCommand(cacheBypass));
            assert.commandWorked(nodeAdminDB.runCommand(getUser));
        });

        originalTeardown(db, collName, cluster);
    };

    return $config;
});
