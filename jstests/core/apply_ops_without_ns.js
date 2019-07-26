// @tags: [
//     requires_non_retryable_commands,
//
//     # applyOps uses the oplog that require replication support
//     requires_replication,
// ]

(function() {
'use strict';

// SERVER-33854: This should fail and not cause any invalid memory access.
assert.commandFailed(db.adminCommand(
    {applyOps: [{'op': 'c', 'ns': 'admin.$cmd', 'o': {applyOps: [{'op': 'i', 'o': {x: 1}}]}}]}));
})();
