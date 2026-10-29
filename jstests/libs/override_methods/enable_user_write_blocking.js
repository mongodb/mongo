/**
 * Enables global user write blocking for the duration of an authenticated passthrough suite.
 *
 * The suite authenticates as __system, which has bypassWriteBlockingMode, so privileged test
 * operations continue to exercise the command paths while user writes are blocked.
 */

assert.commandWorked(db.getSiblingDB("admin").runCommand({setUserWriteBlockMode: 1, global: true}));
