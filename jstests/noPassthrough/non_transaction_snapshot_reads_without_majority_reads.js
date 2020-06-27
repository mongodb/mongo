/* Tests readConcern level snapshot outside of transactions is not supported when
 * enableMajorityReadConcern is false.
 *
 * @tags: [
 *   requires_fcv_46,
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

const replSet = new ReplSetTest({nodes: [{"enableMajorityReadConcern": "false"}]});

replSet.startSet();
replSet.initiate();

const primary = replSet.getPrimary();

// Tests that snapshot reads return error code ReadConcernMajorityNotEnabled.
assert.commandFailedWithCode(
    primary.getDB('test').runCommand({find: "foo", readConcern: {level: "snapshot"}}),
    ErrorCodes.ReadConcernMajorityNotEnabled);

replSet.stopSet();
})();
