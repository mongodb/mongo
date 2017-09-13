/* test removing a node from a replica set
 *
 * Start set with two nodes
 * Initial sync
 * Remove secondary
 * Bring secondary back up
 * Add it back as secondary
 * Make sure both nodes are either primary or secondary
 */

load("jstests/replsets/rslib.js");
var name = "removeNodes";
var host = getHostName();

print("Start set with two nodes");
var replTest = new ReplSetTest({name: name, nodes: 2});
var nodes = replTest.startSet();
replTest.initiate();
var master = replTest.getPrimary();
var secondary = replTest.getSecondary();

print("Initial sync");
master.getDB("foo").bar.baz.insert({x: 1});

replTest.awaitReplication();

print("Remove secondary");
var config = replTest.getReplSetConfig();
for (var i = 0; i < config.members.length; i++) {
    if (config.members[i].host == secondary.host) {
        config.members.splice(i, 1);
        break;
    }
}
var nextVersion = replTest.getReplSetConfigFromNode().version + 1;
config.version = nextVersion;

assert.eq(secondary.getDB("admin").runCommand({ping: 1}).ok,
          1,
          "we should be connected to the secondary");

try {
    master.getDB("admin").runCommand({replSetReconfig: config});
} catch (e) {
    print(e);
}

// This tests that the secondary disconnects us when it picks up the new config.
assert.soon(function() {
    try {
        secondary.getDB("admin").runCommand({ping: 1});
    } catch (e) {
        return true;
    }
    return false;
});

// Now we should successfully reconnect to the secondary.
assert.eq(
    secondary.getDB("admin").runCommand({ping: 1}).ok, 1, "we aren't connected to the secondary");

reconnect(master);

assert.soon(function() {
    var c = master.getDB("local").system.replset.findOne();
    return c.version == nextVersion;
});

print("Add it back as a secondary");
config.members.push({_id: 2, host: secondary.host});
nextVersion++;
config.version = nextVersion;
// Need to keep retrying reconfig here, as it will not work at first due to the primary's
// perception that the secondary is still "down".
assert.soon(function() {
    try {
        reconfig(replTest, config);
        return true;
    } catch (e) {
        return false;
    }
});
master = replTest.getPrimary();
secondary = replTest.getSecondary();
printjson(master.getDB("admin").runCommand({replSetGetStatus: 1}));
var newConfig = master.getDB("local").system.replset.findOne();
print("newConfig: " + tojson(newConfig));
assert.eq(newConfig.version, nextVersion);

print("reconfig with minority");
replTest.stop(secondary);

assert.soon(function() {
    try {
        return master.getDB("admin").runCommand({isMaster: 1}).secondary;
    } catch (e) {
        print("trying to get master: " + e);
    }
}, "waiting for primary to step down", (60 * 1000), 1000);

nextVersion++;
config.version = nextVersion;
config.members = config.members.filter(node => node.host == master.host);
try {
    master.getDB("admin").runCommand({replSetReconfig: config, force: true});
} catch (e) {
    print(e);
}

reconnect(master);
assert.soon(function() {
    return master.getDB("admin").runCommand({isMaster: 1}).ismaster;
}, "waiting for old primary to accept reconfig and step up", (60 * 1000), 1000);

config = master.getDB("local").system.replset.findOne();
printjson(config);
assert.gt(config.version, nextVersion);

replTest.stopSet();
