/**
 * Tests that a shard identity document cannot be inserted or updated with a shardName that is not
 * allowed for the server's cluster role.
 *
 * @tags: [
 *   requires_fcv_80,
 *   does_not_support_stepdowns,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2, configShard: true});
const rs = new ReplSetTest({name: "new-shard-rs", nodes: 1, nodeOptions: {shardsvr: ""}});

rs.startSet();
rs.initiate();

let configConnStr = st.configRS.getURL();

let shardIdentityDoc = {
    _id: "shardIdentity",
    configsvrConnectionString: configConnStr,
    shardName: "config",
    clusterId: ObjectId(),
};

const updateErrorMsgPrefix = "Plan executor error during update :: caused by :: ";

// TODO: SERVER-67837 Change to test that the server crashed when auto-bootstrapping is enabled by
// default.
//
// Insert with shard name "config" on shard server should fail
let res = assert.commandFailedWithCode(
    rs
        .getPrimary()
        .getDB("admin")
        .runCommand({insert: "system.version", documents: [shardIdentityDoc]}),
    ErrorCodes.UnsupportedFormat,
);
assert.eq(
    res.writeErrors[0].errmsg,
    'Invalid shard identity document: the shard name for a shard server cannot be "config"',
);

// Update with shard name "config" on shard server should fail
res = assert.commandFailedWithCode(
    st.shard1
        .getDB("admin")
        .runCommand({update: "system.version", updates: [{q: {"_id": "shardIdentity"}, u: shardIdentityDoc}]}),
    ErrorCodes.UnsupportedFormat,
);
assert.eq(
    res.writeErrors[0].errmsg,
    updateErrorMsgPrefix + 'Invalid shard identity document: the shard name for a shard server cannot be "config"',
);

// Update with shard name "pizza" on config server should fail
shardIdentityDoc.shardName = "pizza";
res = assert.commandFailedWithCode(
    st.shard0
        .getDB("admin")
        .runCommand({update: "system.version", updates: [{q: {"_id": "shardIdentity"}, u: shardIdentityDoc}]}),
    ErrorCodes.UnsupportedFormat,
);
assert.eq(
    res.writeErrors[0].errmsg,
    updateErrorMsgPrefix + 'Invalid shard identity document: the shard name for a config server cannot be "pizza"',
);

// TODO SERVER-74570: Enable parallel shutdown
st.stop({parallelSupported: false});
rs.stopSet();
