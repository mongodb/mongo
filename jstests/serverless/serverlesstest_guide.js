/*
 * @tags: [serverless]
 */
(function() {
"use strict";

load("jstests/serverless/serverlesstest.js");

let st = new ServerlessTest();

(() => {
    jsTest.log("Test adding and removing tenants to/from config.tenants");
    const tenantID = ObjectId();
    assert.eq(0, st.removeTenant(tenantID.str, st.shard0.shardName).nRemoved);
    assert.eq(1, st.addTenant(tenantID.str, st.shard0.shardName).nInserted);
    assert.eq(1, st.removeTenant(tenantID.str, st.shard0.shardName).nRemoved);
    assert.eq(0, st.removeTenant(tenantID.str, st.shard0.shardName).nRemoved);
})();

st.stop();
})();
