/**
 * Tests that basic validation within the configureQueryAnalyzer command.
 *
 * @tags: [requires_fcv_70]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    runInvalidNamespaceTestsForConfigure
} from "jstests/sharding/analyze_shard_key/libs/validation_common.js";

{
    const st = new ShardingTest({shards: 1});
    runInvalidNamespaceTestsForConfigure(st.s);
    st.stop();
}

if (!jsTestOptions().useAutoBootstrapProcedure) {  // TODO: SERVER-80318 Remove block
    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();
    runInvalidNamespaceTestsForConfigure(primary);
    rst.stopSet();
}
