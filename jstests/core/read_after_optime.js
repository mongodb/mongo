// Test that attempting to read after optime fails if replication is not enabled.

// @tags: [
//     # readConcern code is stripped in embedded and that causes this to succeed.
//     # TODO SERVER-34577
//     incompatible_with_embedded_todo_investigate
// ]

(function() {
    "use strict";

    var currentTime = new Date();

    var futureOpTime = new Timestamp((currentTime / 1000 + 3600), 0);

    assert.commandFailedWithCode(
        db.runCommand(
            {find: 'user', filter: {x: 1}, readConcern: {afterOpTime: {ts: futureOpTime, t: 0}}}),
        ErrorCodes.NotAReplicaSet);
})();
