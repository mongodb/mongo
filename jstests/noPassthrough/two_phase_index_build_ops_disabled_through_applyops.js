/**
 * Ensures that oplog entries specific to two-phase index builds are not allow when run through
 * applyOps.
 *
 * @tags: [requires_replication]
 */

(function() {

    const replSet = new ReplSetTest({
        nodes: [
            {},
            {
              // Disallow elections on secondary.
              rsConfig: {
                  priority: 0,
                  votes: 0,
              },
            },
        ]
    });

    replSet.startSet();
    replSet.initiate();

    const testDB = replSet.getPrimary().getDB('test');
    const coll = testDB.twoPhaseIndexBuild;
    const cmdNs = testDB.getName() + ".$cmd";

    coll.insert({a: 1});

    assert.commandFailedWithCode(testDB.adminCommand({
        applyOps: [
            {op: "c", ns: cmdNs, o: {startIndexBuild: coll.getName(), key: {a: 1}, name: 'a_1'}}
        ]
    }),
                                 [ErrorCodes.CommandNotSupported, ErrorCodes.FailedToParse]);

    assert.commandFailedWithCode(testDB.adminCommand({
        applyOps: [{
            op: "c",
            ns: cmdNs,
            o: {commitIndexBuild: coll.getName(), key: {a: 1}, name: 'a_1'}
        }]
    }),
                                 [ErrorCodes.CommandNotSupported, ErrorCodes.FailedToParse]);

    assert.commandFailedWithCode(testDB.adminCommand({
        applyOps: [
            {op: "c", ns: cmdNs, o: {abortIndexBuild: coll.getName(), key: {a: 1}, name: 'a_1'}}
        ]
    }),
                                 [ErrorCodes.CommandNotSupported, ErrorCodes.FailedToParse]);

    replSet.stopSet();
})();
