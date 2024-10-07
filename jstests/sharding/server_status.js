/**
 * Basic test for the 'sharding' section of the serverStatus response object for
 * both mongos and the shard.
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

var st = new ShardingTest({shards: 1});

var testDB = st.s.getDB('test');
testDB.adminCommand({enableSharding: 'test'});
testDB.adminCommand({shardCollection: 'test.user', key: {_id: 1}});

// Initialize shard metadata in shards
testDB.user.insert({x: 1});

var checkShardingServerStatus = function(doc, role) {
    var shardingSection = doc.sharding;
    assert.neq(shardingSection, null);

    var configConnStr = shardingSection.configsvrConnectionString;
    var configConn = new Mongo(configConnStr);
    var configHello = configConn.getDB('admin').runCommand({hello: 1});

    var configOpTimeObj = shardingSection.lastSeenConfigServerOpTime;

    let commitTypesSection = doc.transactions.commitTypes;
    let opCountersReplSection = doc.opcountersRepl;

    if (role === "router") {
        // Router role should have the commitTypes section, but not the opCountersRepl section.
        assert.neq(commitTypesSection, undefined);
        assert(commitTypesSection.hasOwnProperty("noShards"), tojson(commitTypesSection));
        assert(commitTypesSection.hasOwnProperty("singleShard"), tojson(commitTypesSection));

        assert.eq(opCountersReplSection, undefined);
    } else {
        assert.eq(role, "shard");
        assert.neq(opCountersReplSection, undefined);
        assert(opCountersReplSection.hasOwnProperty("insert"), tojson(opCountersReplSection));

        assert.eq(commitTypesSection, undefined);
    }

    assert.gt(configConnStr.indexOf('/'), 0);
    assert.gte(configHello.configsvr, 1);  // If it's a shard, this field won't exist.
    assert.neq(null, configOpTimeObj);
    assert.neq(null, configOpTimeObj.ts);
    assert.neq(null, configOpTimeObj.t);

    assert.neq(null, shardingSection.maxChunkSizeInBytes);
};

var mongosServerStatus = testDB.adminCommand({serverStatus: 1});
checkShardingServerStatus(mongosServerStatus, "router");

var mongodServerStatus = st.rs0.getPrimary().getDB('admin').runCommand({serverStatus: 1});
checkShardingServerStatus(mongodServerStatus, "shard");

st.stop();
