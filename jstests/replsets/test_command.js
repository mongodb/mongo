// Tests 'replSetTest' command:
//    waitForMemberState - waits for node's state to become 'expectedState'.
//    waitForDrainFinish - waits for primary to finish draining its applier queue.

(function() {
    'use strict';
    var name = 'test_command';
    var replSet = new ReplSetTest({name: name, nodes: 3});
    var nodes = replSet.nodeList();
    replSet.startSet();
    replSet.initiate({
        _id: name,
        members: [
            {_id: 0, host: nodes[0], priority: 3},
            {_id: 1, host: nodes[1]},
            {_id: 2, host: nodes[2], arbiterOnly: true},
        ],
    });

    // Stabilize replica set with node 0 as primary.

    assert.commandWorked(replSet.nodes[0].adminCommand({
        replSetTest: 1,
        waitForMemberState: ReplSetTest.State.PRIMARY,
        timeoutMillis: 60 * 1000,
    }),
                         'node 0' + replSet.nodes[0].host + ' failed to become primary');

    // We need the try/catch to handle that the node may have hung up the connection due
    // to a state change.
    try {
        assert.commandWorked(replSet.nodes[1].adminCommand({
            replSetTest: 1,
            waitForMemberState: ReplSetTest.State.SECONDARY,
            timeoutMillis: 60 * 1000,
        }));
    } catch (e) {
        jsTestLog(e);
        assert.commandWorked(replSet.nodes[1].adminCommand({
            replSetTest: 1,
            waitForMemberState: ReplSetTest.State.SECONDARY,
            timeoutMillis: 60 * 1000,
        }),
                             'node 1' + replSet.nodes[1].host + ' failed to become secondary');
    }

    var primary = replSet.getPrimary();
    var secondary = replSet.getSecondary();

    // Check replication mode.

    assert.commandFailedWithCode(primary.getDB(name).runCommand({
        replSetTest: 1,
    }),
                                 ErrorCodes.Unauthorized,
                                 'replSetTest should fail against non-admin database');

    assert.commandWorked(primary.adminCommand({
        replSetTest: 1,
    }),
                         'failed to check replication mode');

    // waitForMemberState tests.

    assert.commandFailedWithCode(
        primary.adminCommand({
            replSetTest: 1,
            waitForMemberState: 'what state',
            timeoutMillis: 1000,
        }),
        ErrorCodes.TypeMismatch,
        'replSetTest waitForMemberState should fail on non-numerical state');

    assert.commandFailedWithCode(
        primary.adminCommand({
            replSetTest: 1,
            waitForMemberState: ReplSetTest.State.PRIMARY,
            timeoutMillis: "what timeout",
        }),
        ErrorCodes.TypeMismatch,
        'replSetTest waitForMemberState should fail on non-numerical timeout');

    assert.commandFailedWithCode(primary.adminCommand({
        replSetTest: 1,
        waitForMemberState: 9999,
        timeoutMillis: 1000,
    }),
                                 ErrorCodes.BadValue,
                                 'replSetTest waitForMemberState should fail on invalid state');

    assert.commandFailedWithCode(primary.adminCommand({
        replSetTest: 1,
        waitForMemberState: ReplSetTest.State.PRIMARY,
        timeoutMillis: -1000,
    }),
                                 ErrorCodes.BadValue,
                                 'replSetTest waitForMemberState should fail on negative timeout');

    assert.commandFailedWithCode(
        primary.adminCommand({
            replSetTest: 1,
            waitForMemberState: ReplSetTest.State.SECONDARY,
            timeoutMillis: 1000,
        }),
        ErrorCodes.ExceededTimeLimit,
        'replSetTest waitForMemberState(SECONDARY) should time out on node 0 ' + primary.host);

    assert.commandWorked(
        secondary.adminCommand({
            replSetTest: 1,
            waitForMemberState: ReplSetTest.State.SECONDARY,
            timeoutMillis: 1000,
        }),
        'replSetTest waitForMemberState(SECONDARY) failed on node 1 ' + secondary.host);

    // waitForDrainFinish tests.

    assert.commandFailedWithCode(
        primary.adminCommand({
            replSetTest: 1,
            waitForDrainFinish: 'what state',
        }),
        ErrorCodes.TypeMismatch,
        'replSetTest waitForDrainFinish should fail on non-numerical timeout');

    assert.commandFailedWithCode(primary.adminCommand({
        replSetTest: 1,
        waitForDrainFinish: -1000,
    }),
                                 ErrorCodes.BadValue,
                                 'replSetTest waitForDrainFinish should fail on negative timeout');

    assert.commandWorked(primary.adminCommand({
        replSetTest: 1,
        waitForDrainFinish: 1000,
    }),
                         'node 0' + primary.host + ' failed to wait for drain to finish');

    assert.commandWorked(secondary.adminCommand({
        replSetTest: 1,
        waitForDrainFinish: 0,
    }),
                         'node 1' + primary.host + ' failed to wait for drain to finish');
})();
