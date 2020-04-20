/* Tests readConcern level snapshot outside of transactions is not supported when
 * enableMajorityReadConcern is false.
 *
 * TODO(SERVER-46592): This test is multiversion-incompatible in 4.6.  If we use 'requires_fcv_46'
 *                     as the tag for that, removing 'requires_fcv_44' is sufficient.  Otherwise,
 *                     please set the appropriate tag when removing 'requires_fcv_44'
 * @tags: [requires_replication, requires_fcv_44, requires_fcv_46]
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
