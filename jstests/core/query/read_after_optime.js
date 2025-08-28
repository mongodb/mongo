// Test that attempting to read after optime fails if replication is not enabled.
// @tags: [
//   multiversion_incompatible,
//   assumes_standalone_mongod,
// ]

let currentTime = new Date();

let futureOpTime = new Timestamp(currentTime / 1000 + 3600, 0);

assert.commandFailedWithCode(
    db.runCommand({find: "user", filter: {x: 1}, readConcern: {afterOpTime: {ts: futureOpTime, t: 0}}}),
    [ErrorCodes.NotAReplicaSet, ErrorCodes.NotImplemented],
);
