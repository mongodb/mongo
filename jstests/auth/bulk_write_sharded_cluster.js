/*
 * Auth test for the bulkWrite command on sharded clusters.
 * @tags: [
 * requires_sharding,
 * # TODO SERVER-52419 Remove this tag.
 * featureFlagBulkWriteCommand
 * ]
 */
import {runTest} from "jstests/auth/lib/bulk_write_base.js";

const st =
    new ShardingTest({shards: 1, mongos: 1, config: 1, other: {keyFile: 'jstests/libs/key1'}});
runTest(st.s0);
st.stop();
