/*
 * The test runs commands that are not allowed with security token: applyOps.
 * @tags: [
 *   not_allowed_with_security_token,
 *   requires_non_retryable_commands,
 *   # applyOps is not supported on mongos
 *   assumes_against_mongod_not_mongos,
 *   # applyOps uses the oplog that require replication support
 *   requires_replication,
 *   # Tenant migrations don't support applyOps.
 *   tenant_migration_incompatible,
 * ]
 */

(function() {
'use strict';

// SERVER-33854: This should fail and not cause any invalid memory access.
assert.commandFailed(db.adminCommand(
    {applyOps: [{'op': 'c', 'ns': 'admin.$cmd', 'o': {applyOps: [{'op': 'i', 'o': {x: 1}}]}}]}));
})();
