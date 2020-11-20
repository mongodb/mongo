//
// Tests to validate that correct read concern is used to load clusterTime signing keys from
// admin.system.keys on upgrade.
//

load('./jstests/multiVersion/libs/multi_rs.js');

var oldVersion = "last-stable";

var nodes = {
    n1: {binVersion: oldVersion},
    n2: {binVersion: oldVersion},
    n3: {binVersion: oldVersion}
};

function authAllNodes(rst) {
    for (let node of rst.nodes) {
        assert.eq(1, node.getDB("admin").auth("root", "root"));
    }
}

var keyFile = "jstests/libs/key1";
var rst = new ReplSetTest({nodes: nodes, waitForKeys: false, nodeOptions: {keyFile: keyFile}});

rst.startSet();

rst.initiateWithAnyNodeAsPrimary(
    Object.extend(rst.getReplSetConfig(), {writeConcernMajorityJournalDefault: true}));

// Wait for a primary node...
var primary = rst.getPrimary();

primary.getDB("admin").createUser({user: "root", pwd: "root", roles: ["root"]}, {w: 3});

authAllNodes(rst);
primary.getDB("admin").createRole({
    role: "systemkeys",
    privileges: [{resource: {db: "admin", collection: "system.keys"}, actions: ["find"]}],
    roles: []
},
                                  {w: 3});
primary.getDB("admin").grantRolesToUser("root", ["systemkeys"], {w: 3});

assert.soonNoExcept(function(timeout) {
    var keyCnt = primary.getCollection('admin.system.keys').find({purpose: 'HMAC'}).itcount();
    return keyCnt >= 2;
}, "Awaiting keys", 5 * 1000);

var rsConn = new Mongo(rst.getURL());
assert.eq(1, rsConn.getDB("admin").auth("root", "root"));
assert.commandWorked(rsConn.adminCommand({isMaster: 1}));
print("clusterTime: " + tojson(rsConn.getDB("admin").getSession().getClusterTime()));

jsTest.log("Upgrading replica set...");

rst.upgradeSet({keyFile: keyFile, binVersion: "latest"}, "root", "root");

jsTest.log("Replica set upgraded.");

try {
    rsConn.adminCommand({isMaster: 1});
} catch (e) {
}
assert.eq(1, rsConn.getDB("admin").auth("root", "root"));
assert.commandWorked(rsConn.adminCommand({isMaster: 1}));
print("clusterTime2: " + tojson(rsConn.getDB("admin").getSession().getClusterTime()));

rst.stopSet();