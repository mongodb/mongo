/*
 * @tags: [serverless]
 */

import {ShardedServerlessTest} from "jstests/serverless/libs/sharded_serverless_test.js";

let st = new ShardedServerlessTest();

(() => {
    jsTest.log("Test adding and removing tenants to/from config.tenants");
    const tenantID = ObjectId();
    assert.eq(0, st.removeTenant(tenantID.str, st.shard0.shardName).nRemoved);
    assert.eq(1, st.addTenant(tenantID.str, st.shard0.shardName).nInserted);
    assert.eq(1, st.removeTenant(tenantID.str, st.shard0.shardName).nRemoved);
    assert.eq(0, st.removeTenant(tenantID.str, st.shard0.shardName).nRemoved);
})();

st.stop();
