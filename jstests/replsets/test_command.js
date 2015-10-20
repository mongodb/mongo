// Tests 'replSetTest' command:
//    waitForMemberState - waits for node's state to become 'expectedState'.
//    waitForDrainFinish - waits for primary to finish draining its applier queue.

(function () {
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

    assert.commandFailedWithCode(
        replSet.nodes[0].getDB(name).runCommand({
            replSetTest: 1,
            waitForMemberState: replSet.PRIMARY,
            timeoutMillis: 60 * 1000,
        }),
        ErrorCodes.Unauthorized,
        'replSetTest should fail against non-admin database'
    );

    assert.commandWorked(
        replSet.nodes[0].adminCommand({
            replSetTest: 1,
        }),
        'failed to check replication mode'
    );

    // waitForMemberState tests.

    assert.commandFailedWithCode(
        replSet.nodes[0].adminCommand({
            replSetTest: 1,
            waitForMemberState: 'what state',
            timeoutMillis: 1000,
        }),
        ErrorCodes.TypeMismatch,
        'replSetTest waitForMemberState should fail on non-numerical state'
    );

    assert.commandFailedWithCode(
        replSet.nodes[0].adminCommand({
            replSetTest: 1,
            waitForMemberState: replSet.PRIMARY,
            timeoutMillis: "what timeout",
        }),
        ErrorCodes.TypeMismatch,
        'replSetTest waitForMemberState should fail on non-numerical timeout'
    );

    assert.commandFailedWithCode(
        replSet.nodes[0].adminCommand({
            replSetTest: 1,
            waitForMemberState: 9999,
            timeoutMillis: 1000,
        }),
        ErrorCodes.BadValue,
        'replSetTest waitForMemberState should fail on invalid state'
    );

    assert.commandFailedWithCode(
        replSet.nodes[0].adminCommand({
            replSetTest: 1,
            waitForMemberState: replSet.PRIMARY,
            timeoutMillis: -1000,
        }),
        ErrorCodes.BadValue,
        'replSetTest waitForMemberState should fail on negative timeout'
    );

    assert.commandWorked(
        replSet.nodes[0].adminCommand({
            replSetTest: 1,
            waitForMemberState: replSet.PRIMARY,
            timeoutMillis: 60 * 1000,
        }),
        'node 0' + replSet.nodes[0] + ' failed to become primary'
    );

    assert.commandWorked(
        replSet.nodes[1].adminCommand({
            replSetTest: 1,
            waitForMemberState: replSet.SECONDARY,
            timeoutMillis: 1000,
        }),
        ErrorCodes.ExceededTimeLimit,
        'replSetTest waitForMemberState(SECONDARY) failed on node 1 ' +
        replSet.nodes[1]
    );

    assert.commandFailedWithCode(
        replSet.nodes[0].adminCommand({
            replSetTest: 1,
            waitForMemberState: replSet.SECONDARY,
            timeoutMillis: 1000,
        }),
        ErrorCodes.ExceededTimeLimit,
        'replSetTest waitForMemberState(SECONDARY) should time out on node 0 ' +
        replSet.nodes[0]
    );

    // waitForDrainFinish tests.

    assert.commandFailedWithCode(
        replSet.nodes[0].adminCommand({
            replSetTest: 1,
            waitForDrainFinish: 'what state',
        }),
        ErrorCodes.TypeMismatch,
        'replSetTest waitForDrainFinish should fail on non-numerical timeout'
    );

    assert.commandFailedWithCode(
        replSet.nodes[0].adminCommand({
            replSetTest: 1,
            waitForDrainFinish: -1000,
        }),
        ErrorCodes.BadValue,
        'replSetTest waitForDrainFinish should fail on negative timeout'
    );

    assert.commandWorked(
        replSet.nodes[0].adminCommand({
            replSetTest: 1,
            waitForDrainFinish: 1000,
        }),
        'node 0' + replSet.nodes[0] + ' failed to wait for drain to finish'
    );

    assert.commandWorked(
        replSet.nodes[1].adminCommand({
            replSetTest: 1,
            waitForDrainFinish: 0,
        }),
        'node 1' + replSet.nodes[0] + ' failed to wait for drain to finish'
    );
 })();
