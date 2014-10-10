// Test that the replica set member can find itself if on a different port using
// mongobridge
var rt = new ReplSetTest({ name: 'isselfDifferentPortTest', nodes: 1 });
var nodes = rt.startSet({ oplogSize: "2" });
rt.initiate();

jsTestLog("Bridging replica set");
var br = new ReplSetBridge(rt, 0, 0);

jsTestLog("Reconfig to use bridge address");
var config = rt.getPrimary().getDB("local").system.replset.findOne();
config.members[0].host = br.host;
config.version++;

try {
    rt.initiate(config, 'replSetReconfig', 200);
}
catch (ex) {
    // If the reconfig causes connections to close, check that the error is due to the reconfig, not
    // error.  Error message is from JS engine - no error code available unfortunately.
    assert(ex.message.match("error doing query"), ex.message);
}

jsTestLog("Ensure valid set");
var status = rt.status();
assert.commandWorked(status)
printjson(status)

assert.eq(br.host, status.members[0].name, "host should be the bridge address")

jsTestLog("Test Finished");
rt.stopSet();

