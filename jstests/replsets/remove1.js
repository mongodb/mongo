/**test removing a node from a replica set
 *
 * Start set with two nodes
 * Initial sync
 * Remove secondary
 * Bring secondary back up
 * Add it back as secondary
 * Make sure both nodes are either primary or secondary
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {reconnect} from "jstests/replsets/rslib.js";

let name = "removeNodes";
let host = getHostName();

print("Start set with two nodes");
let replTest = new ReplSetTest({name: name, nodes: 2});
let nodes = replTest.startSet();
replTest.initiate(null, null, {initiateWithDefaultElectionTimeout: true});
let primary = replTest.getPrimary();
let secondary = replTest.getSecondary();

print("Initial sync");
primary.getDB("foo").bar.baz.insert({x: 1});

replTest.awaitReplication();

print("Remove secondary");
let config = replTest.getReplSetConfigFromNode(0);
for (let i = 0; i < config.members.length; i++) {
    if (config.members[i].host == secondary.host) {
        config.members.splice(i, 1);
        break;
    }
}
let nextVersion = replTest.getReplSetConfigFromNode().version + 1;
config.version = nextVersion;

assert.eq(secondary.getDB("admin").runCommand({ping: 1}).ok, 1, "we should be connected to the secondary");

try {
    primary.getDB("admin").runCommand({replSetReconfig: config});
} catch (e) {
    print(e);
}

// This tests that the secondary disconnects us when it picks up the new config.
assert.soon(function () {
    try {
        secondary.getDB("admin").runCommand({ping: 1});
    } catch (e) {
        return true;
    }
    return false;
});

// Now we should successfully reconnect to the secondary.
assert.eq(secondary.getDB("admin").runCommand({ping: 1}).ok, 1, "we aren't connected to the secondary");

reconnect(primary);

assert.soon(function () {
    let c = primary.getDB("local").system.replset.findOne();
    return c.version == nextVersion;
});

print("Add it back as a secondary");
config.members.push({_id: 2, host: secondary.host});
nextVersion++;
config.version = nextVersion;
// Need to keep retrying reconfig here, as it will not work at first due to the primary's
// perception that the secondary is still "down".
assert.soon(function () {
    try {
        assert.commandWorked(replTest.getPrimary().adminCommand({replSetReconfig: config}));
        return true;
    } catch (e) {
        return false;
    }
});
primary = replTest.getPrimary();

// Wait and account for 'newlyAdded' automatic reconfig.
nextVersion++;
replTest.waitForAllNewlyAddedRemovals();

secondary = replTest.getSecondary();
printjson(primary.getDB("admin").runCommand({replSetGetStatus: 1}));
let newConfig = primary.getDB("local").system.replset.findOne();
print("newConfig: " + tojson(newConfig));
assert.eq(newConfig.version, nextVersion);

print("reconfig with minority");
replTest.stop(secondary);

assert.soon(
    function () {
        try {
            return primary.getDB("admin").runCommand({hello: 1}).secondary;
        } catch (e) {
            print("trying to get primary: " + e);
        }
    },
    "waiting for primary to step down",
    60 * 1000,
    1000,
);

nextVersion++;
config.version = nextVersion;
config.members = config.members.filter((node) => node.host == primary.host);
try {
    primary.getDB("admin").runCommand({replSetReconfig: config, force: true});
} catch (e) {
    print(e);
}

reconnect(primary);
assert.soon(
    function () {
        return primary.getDB("admin").runCommand({hello: 1}).isWritablePrimary;
    },
    "waiting for old primary to accept reconfig and step up",
    60 * 1000,
    1000,
);

config = primary.getDB("local").system.replset.findOne();
printjson(config);
assert.gt(config.version, nextVersion);

replTest.stopSet();
