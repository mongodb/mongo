// Reconfigure WiredTiger and spill WiredTiger test cases
//
// Start our own instance of mongod so that are settings tests
// do not cause issues for other tests
//
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

var ss = db.serverStatus();

// Test is only valid in the WT suites which run against a mongod with WiredTiger enabled
if (ss.storageEngine.name !== "wiredTiger") {
    print("Skipping reconfigwt.js since this server does not have WiredTiger enabled");
} else {
    var conn = MongoRunner.runMongod();

    var admin = conn.getDB("admin");

    function runTestForParam(paramName) {
        function reconfigure(str) {
            let ret = admin.runCommand({setParameter: 1, [paramName]: str});
            print("ret: " + tojson(ret));
            return ret;
        }

        // See the WT_CONNECTION:reconfigure documentation for a list of valid options
        // http://source.wiredtiger.com/develop/struct_w_t___c_o_n_n_e_c_t_i_o_n.html#a579141678af06217b22869cbc604c6d4
        assert.commandWorked(reconfigure("eviction_target=81"));
        assert.eq("eviction_target=81",
                  admin.adminCommand({getParameter: 1, [paramName]: 1})[paramName]);
        assert.commandWorked(reconfigure("cache_size=81M"));
        assert.eq("cache_size=81M",
                  admin.adminCommand({getParameter: 1, [paramName]: 1})[paramName]);
        assert.commandWorked(reconfigure(
            "eviction_dirty_target=19"));  // must be lower than eviction_dirty_trigger (default 20)
        assert.commandWorked(
            reconfigure("shared_cache=(chunk=11MB, name=bar, reserve=12MB, size=1G)"));

        // Negative tests - bad input to mongod
        assert.commandFailed(reconfigure("abc\0as"));

        // Negative tests - bad input to wt
        assert.commandFailed(reconfigure("eviction_target=a"));
        assert.commandFailed(reconfigure("fake_config_key=1"));
        assert.eq("shared_cache=(chunk=11MB, name=bar, reserve=12MB, size=1G)",
                  admin.adminCommand({getParameter: 1, [paramName]: 1})[paramName]);
    }

    runTestForParam("wiredTigerEngineRuntimeConfig");
    // TODO SERVER-76014: Remove this once FeatureFlagCreateSpillKVEngine is enabled by default.
    if (FeatureFlagUtil.isPresentAndEnabled(db, "FeatureFlagCreateSpillKVEngine")) {
        runTestForParam("spillWiredTigerEngineRuntimeConfig");
    }

    MongoRunner.stopMongod(conn);
}
