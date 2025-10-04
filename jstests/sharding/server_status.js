/**
 * Basic test for the 'sharding' section of the serverStatus response object for
 * both mongos and the shard.
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

let st = new ShardingTest({shards: 1});

let testDB = st.s.getDB("test");
testDB.adminCommand({enableSharding: "test"});
testDB.adminCommand({shardCollection: "test.user", key: {_id: 1}});

// Initialize shard metadata in shards
testDB.user.insert({x: 1});

let checkShardingServerStatus = function (doc, role) {
    let shardingSection = doc.sharding;
    assert.neq(shardingSection, null);

    let configConnStr = shardingSection.configsvrConnectionString;
    let configConn = new Mongo(configConnStr);
    let configHello = configConn.getDB("admin").runCommand({hello: 1});

    let configOpTimeObj = shardingSection.lastSeenConfigServerOpTime;

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

    assert.gt(configConnStr.indexOf("/"), 0);
    assert.gte(configHello.configsvr, 1); // If it's a shard, this field won't exist.
    assert.neq(null, configOpTimeObj);
    assert.neq(null, configOpTimeObj.ts);
    assert.neq(null, configOpTimeObj.t);

    assert.neq(null, shardingSection.maxChunkSizeInBytes);
};

let mongosServerStatus = testDB.adminCommand({serverStatus: 1});
checkShardingServerStatus(mongosServerStatus, "router");

let mongodServerStatus = st.rs0.getPrimary().getDB("admin").runCommand({serverStatus: 1});
checkShardingServerStatus(mongodServerStatus, "shard");

st.stop();
