/**
 * Verify that invalid inputs to the cluster count command will throw an overflow error.
 *
 * @tags: [
 *   requires_replication,
 *   requires_sharding,
 * ]
 */

const st = new ShardingTest({mongos: 1, shards: 1, config: 1});
const conn = st.rs0.getPrimary();

assert.commandWorked(conn.adminCommand(
    {clusterCount: "x", skip: 200, limit: 20},
    ));
assert.commandFailedWithCode(conn.adminCommand(
                                 {clusterCount: "x", skip: Infinity, limit: 20},
                                 ),
                             ErrorCodes.Overflow);

st.stop();
