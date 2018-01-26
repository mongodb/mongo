/**
 *  Starts standalone RS with skipShardingConfigurationChecks.
 *  @tags: [requires_persistence]
 */
(function() {
    'use strict';

    function expectState(rst, state) {
        assert.soon(function() {
            var status = rst.status();
            if (status.myState != state) {
                print("Waiting for state " + state + " in replSetGetStatus output: " +
                      tojson(status));
            }
            return status.myState == state;
        });
    }

    let configSvr = MongoRunner.runMongod(
        {configsvr: "", setParameter: 'skipShardingConfigurationChecks=true'});
    assert.eq(configSvr, null);

    let shardSvr =
        MongoRunner.runMongod({shardsvr: "", setParameter: 'skipShardingConfigurationChecks=true'});
    assert.eq(shardSvr, null);

    var st = new ShardingTest({name: "skipConfig", shards: {rs0: {nodes: 1}}});
    var configRS = st.configRS;
    var shardRS = st.rs0;

    st.stopAllMongos();
    shardRS.stopSet(15, true);
    configRS.stopSet(undefined, true);

    jsTestLog("Restarting configRS as a standalone ReplicaSet");

    for (let i = 0; i < configRS.nodes.length; i++) {
        delete configRS.nodes[i].fullOptions.configsvr;
        configRS.nodes[i].fullOptions.setParameter = 'skipShardingConfigurationChecks=true';
    }
    configRS.startSet({}, true);
    expectState(configRS, ReplSetTest.State.PRIMARY);
    configRS.stopSet();

    jsTestLog("Restarting shardRS as a standalone ReplicaSet");
    for (let i = 0; i < shardRS.nodes.length; i++) {
        delete shardRS.nodes[i].fullOptions.shardsvr;
        shardRS.nodes[i].fullOptions.setParameter = 'skipShardingConfigurationChecks=true';
    }
    shardRS.startSet({}, true);
    expectState(shardRS, ReplSetTest.State.PRIMARY);
    shardRS.stopSet();
})();
