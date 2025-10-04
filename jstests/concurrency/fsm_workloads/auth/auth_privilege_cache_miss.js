/**
 * auth_privilege_cache_miss.js
 *
 * Validate user permission consistency during cache miss and slow load.
 *
 * @tags: [
 *  incompatible_with_concurrency_simultaneous,
 *  multiversion_incompatible,
 *  requires_auth,
 * ]
 */

// Use the auth_privilege_consistency workload as a base.
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/auth/auth_privilege_consistency.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    // Override setup() to also set cache-miss and slow load failpoints.
    const kResolveRolesDelayMS = 100;

    const originalSetup = $config.setup;
    const originalTeardown = $config.teardown;

    $config.data.test_name = "auth_privilege_cache_miss";

    $config.setup = function (db, collName, cluster) {
        originalSetup.call(this, db, collName, cluster);

        const cacheBypass = {configureFailPoint: "authUserCacheBypass", mode: "alwaysOn"};
        const getUser = {
            configureFailPoint: "authLocalGetSubRoles",
            mode: "alwaysOn",
            data: {resolveRolesDelayMS: NumberInt(kResolveRolesDelayMS)},
        };

        cluster.executeOnMongosNodes(function (nodeAdminDB) {
            assert.commandWorked(nodeAdminDB.runCommand(cacheBypass));
        });

        cluster.executeOnMongodNodes(function (nodeAdminDB) {
            assert.commandWorked(nodeAdminDB.runCommand(cacheBypass));
            assert.commandWorked(nodeAdminDB.runCommand(getUser));
        });
    };

    $config.teardown = function (db, collName, cluster) {
        const cacheBypass = {configureFailPoint: "authUserCacheBypass", mode: "off"};
        const getUser = {configureFailPoint: "authLocalGetSubRoles", mode: "off"};

        cluster.executeOnMongosNodes(function (nodeAdminDB) {
            assert.commandWorked(nodeAdminDB.runCommand(cacheBypass));
        });

        cluster.executeOnMongodNodes(function (nodeAdminDB) {
            assert.commandWorked(nodeAdminDB.runCommand(cacheBypass));
            assert.commandWorked(nodeAdminDB.runCommand(getUser));
        });

        originalTeardown.call(this, db, collName, cluster);
    };

    return $config;
});
