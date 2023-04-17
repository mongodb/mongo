/**
 * Tests that the config server connection string in the shard identity document of both the
 * primary and secondary will get updated whenever the config server membership changes.
 * @tags: [requires_persistence]
 */

// Checking UUID consistency involves talking to a shard node, which in this test is shutdown
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

(function() {
"use strict";

load('jstests/replsets/rslib.js');

const notInMultiversionTest = !jsTestOptions().shardMixedBinVersions &&
    jsTestOptions().mongosBinVersion !== "last-lts" &&
    jsTestOptions().mongosBinVersion !== "last-continuous";

var st = new ShardingTest({shards: {rs0: {nodes: 2}}});

// Note: Adding new replica set member by hand because of SERVER-24011.

var newNode =
    MongoRunner.runMongod({configsvr: '', replSet: st.configRS.name, storageEngine: 'wiredTiger'});

var replConfig = st.configRS.getReplSetConfigFromNode();
replConfig.version += 1;
replConfig.members.push({_id: 3, host: newNode.host});

reconfig(st.configRS, replConfig);

/**
 * Returns true if the shardIdentity document has all the replica set member nodes in the
 * expectedConfigStr.
 */
var checkConfigStrUpdated = function(conn, expectedConfigStr) {
    var shardIdentity = conn.getDB('admin').system.version.findOne({_id: 'shardIdentity'});

    var shardConfigsvrStr = shardIdentity.configsvrConnectionString;
    var shardConfigReplName = shardConfigsvrStr.split('/')[0];
    var expectedReplName = expectedConfigStr.split('/')[0];

    assert.eq(expectedReplName, shardConfigReplName);

    var expectedHostList = expectedConfigStr.split('/')[1].split(',');
    var shardConfigHostList = shardConfigsvrStr.split('/')[1].split(',');

    if (expectedHostList.length != shardConfigHostList.length) {
        return false;
    }

    for (var x = 0; x < expectedHostList.length; x++) {
        if (shardConfigsvrStr.indexOf(expectedHostList[x]) == -1) {
            return false;
        }
    }

    return true;
};

var origConfigConnStr = st.configRS.getURL();
var expectedConfigStr = origConfigConnStr + ',' + newNode.host;
assert.soon(function() {
    return checkConfigStrUpdated(st.rs0.getPrimary(), expectedConfigStr);
});

st.rs0.getSecondaries().forEach(secConn => {
    secConn.setSecondaryOk();
    assert.soon(function() {
        return checkConfigStrUpdated(secConn, expectedConfigStr);
    });
});

// Config servers in 7.0 also maintain the connection string in their shard identity document.
// TODO SERVER-75391: Always run config server assertions.
if (notInMultiversionTest) {
    assert.soon(function() {
        return checkConfigStrUpdated(st.configRS.getPrimary(), expectedConfigStr);
    });

    st.configRS.getSecondaries().forEach(secConn => {
        secConn.setSecondaryOk();
        assert.soon(function() {
            return checkConfigStrUpdated(secConn, expectedConfigStr);
        });
    });

    newNode.setSecondaryOk();
    assert.soon(function() {
        return checkConfigStrUpdated(newNode, expectedConfigStr);
    });
}

//
// Remove the newly added member from the config replSet while the shards are down.
// Check that the shard identity document will be updated with the new replSet connection
// string when they come back up.
//

// We can't reconfigure the config server if some nodes are down, so skip in config shard mode and
// just verify all nodes update the config string eventually.
if (!TestData.configShard) {
    st.rs0.stop(0);
    st.rs0.stop(1);
}

MongoRunner.stopMongod(newNode);

replConfig = st.configRS.getReplSetConfigFromNode();
replConfig.version += 1;
replConfig.members.pop();

reconfig(st.configRS, replConfig);

if (!TestData.configShard) {
    st.rs0.restart(0, {shardsvr: ''});
    st.rs0.restart(1, {shardsvr: ''});
}

st.rs0.waitForPrimary();
st.rs0.awaitSecondaryNodes();

assert.soon(function() {
    return checkConfigStrUpdated(st.rs0.getPrimary(), origConfigConnStr);
});

st.rs0.getSecondaries().forEach(secConn => {
    secConn.setSecondaryOk();
    assert.soon(function() {
        return checkConfigStrUpdated(secConn, origConfigConnStr);
    });
});

// Config servers in 7.0 also maintain the connection string in their shard identity document.
// TODO SERVER-75391: Always run config server assertions.
if (notInMultiversionTest) {
    assert.soon(function() {
        return checkConfigStrUpdated(st.configRS.getPrimary(), origConfigConnStr);
    });

    st.configRS.getSecondaries().forEach(secConn => {
        secConn.setSecondaryOk();
        assert.soon(function() {
            return checkConfigStrUpdated(secConn, origConfigConnStr);
        });
    });
}

st.stop();
})();
