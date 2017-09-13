// Test copyDatabase command inside a sharded cluster with and without auth.  Tests with auth are
// currently disabled due to SERVER-13080.

var baseName = "jstests_clone_copyauth_between_shards";

function copydbWithinShardedCluster(useReplSets, passCredentials, useAuth) {
    var clusterConfig = {shards: 1, mongos: 1, config: 1};

    if (useAuth) {
        clusterConfig.auth = "";
        clusterConfig.keyFile = "jstests/libs/key1";
    }

    if (useReplSets) {
        clusterConfig.rs = {};
    }
    var st = new ShardingTest(clusterConfig);

    var mongos = st.s;

    var test1 = mongos.getDB('test1');
    var test2 = mongos.getDB('test2');

    if (useAuth) {
        mongos.getDB("admin").createUser({user: "super", pwd: "super", roles: ["root"]});
        assert.throws(function() {
            mongos.getDB("test1")["test1"].findOne();
        });
        mongos.getDB("admin").auth("super", "super");
    }

    test1.getCollection('test').insert({foo: 'bar'});
    jsTestLog('Test document on source db:');
    printjson(test1.getCollection('test').findOne());
    jsTestLog('copydb');

    // The copyDatabase command acts differently depending on whether we pass username and password
    if (passCredentials) {
        var result =
            mongos.getDB('admin').copyDatabase('test1', 'test2', undefined, "super", "super");
    } else {
        var result = mongos.getDB('admin').copyDatabase('test1', 'test2');
    }
    printjson(result);
    assert.eq(result.ok, 1.0);
    jsTestLog('Test document on destination db:');
    printjson(test2.getCollection('test').findOne());
    st.stop();
}

// SERVER-13080
// copydbWithinShardedCluster(true, true, true);
// copydbWithinShardedCluster(false, true, true);
// copydbWithinShardedCluster(true, false, true);
// copydbWithinShardedCluster(false, false, true);
copydbWithinShardedCluster(true, false, false);
copydbWithinShardedCluster(false, false, false);

print(baseName + " success!");
