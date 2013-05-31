load("jstests/replsets/rslib.js");

// Return the state of "subject" according to the replica set status of "pov".
var getHostStateAccordingToNode = function (nodes, subject, pov) {
    var subjectHost = nodes[subject].host;
    var status = nodes[pov].getDB("admin").runCommand({replSetGetStatus: 1});

    for (var i = 0; i < status.members.length; i++) {
        if (subjectHost === status.members[i].name) {
            return status.members[i].state;
        }
    }

    throw {
        name: 'UnexpectedValueException',
        message: 'Subject host not found in POV status',
        subject: subject,
        subjectHost: subjectHost,
        pov: pov,
        povStatusMembers: status.members
    }
}

/* The third replica node is necessary to avoid a failover. We could certainly
 * allow for a failover, but we would need to change the assertion that Y's
 * status for X never changes.
 */
var replTest = new ReplSetTest({
    name: 'testSet',
    nodes: 3,
    oplogSize: 1
});
var nodes = replTest.startSet();

// Set an explicit value for heartbeatTimeoutSecs
var config = replTest.getReplSetConfig();
config.settings = {};
config.settings.heartbeatTimeoutSecs = 10;
replTest.initiate(config);

var master = replTest.waitForMaster();

waitForAllMembers(master.getDB('admin'));

/* With heartbeatTimeoutSecs configured as 10 seconds, delay Y's heartbeat
 * responses by 30 seconds and ensure X does not change Y's state until after
 * 13 seconds. The 13-second delay is necessary due to the 2-second heartbeat
 * interval. If X just completed a heartbeat before we introduce delay on Y, we
 * need to wait at least 12 seconds (2 + heartbeatTimeoutSecs).
 */
var yStateFromX = getHostStateAccordingToNode(nodes, 1, 0);
var xStateFromY = getHostStateAccordingToNode(nodes, 0, 1);

assert.contains(yStateFromX, [1,2], 'X considers Y to be up');
assert.contains(xStateFromY, [1,2], 'Y considers X to be up');

var result = nodes[1].getDB("admin").runCommand({
    configureFailPoint: 'rsDelayHeartbeatResponse',
    mode: 'alwaysOn',
    data: { delay: 30 }
});
assert.eq(1, result.ok, 'Successfully set rsDelayHeartbeatResponse fail point on Y');

/* While we could wait 5 seconds and ensure that both nodes still see each other
 * in the same state, that may cause issues with slower test environments.
 * Instead, we'll wait the full 13 seconds and ensure X considers Y to be down.
 */
sleep(13000);

assert.eq(8, getHostStateAccordingToNode(nodes, 1, 0), 'X considers Y to be down');
assert.eq(xStateFromY, getHostStateAccordingToNode(nodes, 0, 1),
    'Y considers X to be in the same state');

var result = nodes[1].getDB("admin").runCommand({
    configureFailPoint: 'rsDelayHeartbeatResponse',
    mode: 'off'
});
assert.eq(1, result.ok, 'Successfully cleared rsDelayHeartbeatResponse fail point on Y');

// Reconfigure the replica set with a new heartbeatTimeoutSecs value
var masterConfig = master.getDB("local")['system.replset'].findOne();
config = replTest.getReplSetConfig();
config.settings = {};
config.settings.heartbeatTimeoutSecs = 5;
config.version = masterConfig.version + 1;
replTest.initiate(config, 'replSetReconfig');

master = replTest.waitForMaster();

waitForAllMembers(master.getDB('admin'));

/* With heartbeatTimeoutSecs configured as 5 seconds, have X fail one heartbeat
 * request to Y. Ensure X never change's Y state over a 10-second interval.
 */
yStateFromX = getHostStateAccordingToNode(nodes, 1, 0);
xStateFromY = getHostStateAccordingToNode(nodes, 0, 1);

assert.contains(yStateFromX, [1,2], 'X considers Y to be up');
assert.contains(xStateFromY, [1,2], 'Y considers X to be up');

result = nodes[0].getDB("admin").runCommand({
    configureFailPoint: 'rsStopHeartbeatRequest',
    mode: { times: 1 },
    data: { member: nodes[1].host }
});
assert.eq(1, result.ok, 'Successfully set rsStopHeartbeatRequest fail point on X');

for (var i = 0; i < 10; ++i) {
    assert.eq(yStateFromX, getHostStateAccordingToNode(nodes, 1, 0),
        'X considers Y to be in the same state');
    assert.eq(xStateFromY, getHostStateAccordingToNode(nodes, 0, 1),
        'Y considers X to be in the same state');
    sleep(1000);
}

replTest.stopSet();
